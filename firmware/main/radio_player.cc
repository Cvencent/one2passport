#include "radio_player.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "radio_ui.h"
#include "radio_stream_buffer.h"
#include "xiaozhi_client.h"     // XZ_MIC_TAIL_ARENA_CAP(竞技场容量上限)
#include "radio_mp3_header.h"
#include "radio_retry.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include <algorithm>
#include <atomic>
#include "esp_wifi.h"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <strings.h>

extern "C" uint8_t *xiaozhi_idle_buffer(std::size_t *size);
extern "C" bool xiaozhi_radio_tail_available(void);
extern "C" uint8_t *xiaozhi_radio_tail_claim(std::size_t *size);
extern "C" void xiaozhi_radio_tail_release(void);

namespace {

constexpr char kTag[] = "radio_player";
// ⚠ 输入块大小必须保持 1024: 实测改成 4096 会让 esp_audio_simple_dec_process
// 连续报 "ESP_MP3_DEC: Fail to init MP3 decoder ret 10" + "Decode error reach
// limited 20"(即使内存充裕也不解), 说明该库对单次投喂的块大小敏感。
// 卡顿问题改用 I2S DMA 缓冲 + 任务优先级 + HTTP 接收缓冲解决, 不动这里。
constexpr std::size_t kInputSize = 1024;
// 输出缓冲 = MP3 最大一帧 PCM(4608B) + 预留给网络块的尾部。
// MPEG Layer III 每声道最多 1152 样本, 16bit 立体声即 1152*2ch*2B = 4608B。
constexpr std::size_t kPcmCapacity = 4608;
// 尾部 2KB = 2 个网络块(与输出缓冲共享), 开机随解码器池一起预分配。
// 其余网络块优先借用小智关闭时闲置的 25.6KB 麦克风静态栈(见
// xiaozhi_idle_buffer), 借用不可用时才退回按剩余堆现分配。
//
// 取 2 而不是 6 的原因(2026-10-12 实测): 输出缓冲每多 1KB, 开机就少
// 1KB 可用堆, 到 post_coding_bridge 时 largest 只剩 4~6KB —— 而 HTTP client
// 本身还要分配 ~1KB 连续块, 预分配过头反而 "Allocation failed"。
//
// ⚠⚠ 2026-10-12 回归修复: 上面 kPcmCapacity 曾被写成 kOutputSize, 于是
// kSharedNetworkBlocks 恒等于 0, "与输出缓冲共享" 被静默关闭 —— 播放时 6 个
// 网络块全部现分配 malloc, 而那正是堆碎到 largest<2.4KB 的时刻, UI 重绘
// 拿不到内存 -> LVGL 卡死 -> task_wdt 每 5s 触发一次 -> CPU 满载, 就是
// "一进收音机就发烫" 的根因。static_assert 就是为了让它再也回不来。
// 2026-10-21: 2 -> 1.
//
// 真机实测收音机页(桥接已停)的 dma 空闲地板是 1496B, 而 WiFi 每个动态接收
// 缓冲需要约 1600B **连续** DMA 内存 —— 就差这 100B, 于是收包缓冲周期性
// 分配失败: 设备不 ACK -> 服务器重传退避 -> 约 15s 后主动 RST。日志表现为
// 每 ~17s 一条 `transport_base: tcp_read error, errno=Software caused
// connection abort` + "network feed stopped by read error", 之后重建流又要
// 3.6KB, 堆随之在 7.4K/13K 之间摆动 —— 这就是收音机页"一卡一卡"的真凶
// (与 Codex 无关: 桥接在进入收音机页时已经停掉)。
//
// MP3 解码只需 kPcmCapacity(4608) 作为输出缓冲, 尾部那 2 个 1KB 块纯粹是
// 顺手给环形缓冲用的。砍到 1 块可把 1KB 还给通用堆, 使 dma 地板升到约
// 2500B, 足以稳定供出 1600B 的收包缓冲。环形缓冲只少 1KB(约 5%), 对抖动
// 吸收影响可忽略, 换来的是连接不再被服务器掐断。
constexpr std::size_t kSharedNetworkBlocks = 1;
constexpr std::size_t kOutputSize = kPcmCapacity + kSharedNetworkBlocks * kInputSize;
// 编译期自检: 上面三行一旦被改回 kPcmCapacity == kOutputSize, 共享块会静默
// 归零(播放期改走 malloc, 挤死 UI 缓冲 -> LVGL 卡死 -> 满载发热)。这里让它
// 直接编译失败, 而不是等到真机复现。
static_assert(kPcmCapacity >= 4608,
              "MP3 full stereo frame needs 1152*2ch*2B = 4608B");
static_assert(kSharedNetworkBlocks >= 1,
              "network blocks must share the output pool (see comment above)");
static_assert(kPcmCapacity + kSharedNetworkBlocks * kInputSize <= kOutputSize,
              "shared network blocks must fit inside the output pool");
// MP3 解码器首帧初始化真正要的那块**连续**内部堆。2026-10-25 反汇编
// esp32c3 版 libesp_audio_codec.a 实测: esp_mp3_dec_decode 首次进入时会调用
//   pvmp3_decoderMemRequirements() -> media_lib_module_calloc(..., 0x5978)
// 即一次 22904B(0x5978) 的连续分配。旧值按 20480B 估计偏小, 于是
// largest=22528 时仍会放行一次注定失败的解码, 反复刷
// "insufficient contiguous heap for decoder first frame"。
constexpr std::size_t kDecoderInitBytes = 0x5978;
// 判"重试是否有意义"的阈值: largest 低于此值时重试必然失败, 必须直接交外层
// 退避, 否则会以 ~5 次/秒刷 "Fail to init" 并烧 CPU(实测发热)。向上取整到
// 23KiB, 保证不低于解码器真实需求。
constexpr std::size_t kDecoderMinBlock = 23 * 1024;
// 建立连接(DNS/TCP/TLS + HTTP client)所需的连续内部堆下限。低于它时的 connect
// 失败属于**内存不足**, 不是电台失效 —— 必须原地等待, 绝不能换台。
// 2026-10-25 真机复现: 首页/菜单层合法地只剩 ~1.2KB free 时进收音机,
// getaddrinfo() 返回 202(EAI_MEMORY) -> 旧代码按"电台不可达"处理, 毫秒级把
// 整表电台刷一遍, 日志雪崩 + CPU 打满, 最终 RTC_SW_CPU_RST 复位。
//
// 2026-10-25 (方案 A) 从 6KB 放宽到 2.5KB: 方案 A 让首页播放时停掉桥接,
// 解码池(22.9KB, 仍在堆里)吃掉最大块后首页合法地只剩 ~3.1KB —— 这不是
// "内存不足", 而是**正常可播状态**: 项目实测(2026-10-12, radio_player.cc
// 头部注释) dma 空闲 >=2.5KB 就足以稳定供出 1600B 的 WiFi 收包缓冲,
// getaddrinfo 在 1.2KB 才开始返回 EAI_MEMORY。2.5KB 卡在两者中间:
// 低于它 = 真的连不动(原地退避), 高于它 = 放手去连。
constexpr std::size_t kConnectionMinBlock = 2500;

// True when a connection failure is explained by a starved heap rather than a
// dead station. Callers must retry the same station instead of advancing.
static bool connect_out_of_memory() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) <
           kConnectionMinBlock;
}

static_assert(kDecoderMinBlock >= kDecoderInitBytes,
              "kDecoderMinBlock must cover the measured MP3 decoder init "
              "requirement (0x5978) or stream_station will attempt doomed decodes");
constexpr std::size_t kLevelCount = 18;
// 电台列表容量。兜底表 = 47 个实测可持续的全国台(2026-10-26 普查);
// 地方台按城市动态加载(见 kCityPresetStations)。
// 2026-10-26: 14 -> 47。兜底表 = 全部 47 个实测可持续的全国/网络台
// (2026-10-26 普查 192 台, 逐台测投递速率 vs 内容码率, 余量比<=0.75 且
// 12s 采样无停顿)。RadioStation ≈ 418B, 47 项 ≈ 19.6KB 静态:
// post_wifi_init dma≈59KB, 开机 RF 校准安全线 dma>5748B, 余量充足。
// 地方台不占兜底表 —— 按城市从 kCityPresetStations 动态加载(见下)。
// 2026-10-26 (v1.2.14): 47 -> 20。兜底表 = 用户点选的 15 个全国台(显示名
// 按用户指定), 剩余 5 个槽位给城市预设/目录搜索动态加载。
// RadioStation ≈ 418B, 20 项 ≈ 8.4KB 静态(开机 RF 校准安全线内)。
constexpr std::size_t kMaxStations = 20;
// Consecutive silent failures tolerated before moving to the next station.
// Public directories are full of dead entries; retrying the same address just
// makes every "next station" press feel frozen. Move on after the first
// terminal failure, while low-memory failures still hold the same station.
constexpr uint8_t kMaxFailedAttempts = 1;

// These national streams are not tied to one local transmitter, so they carry
// no dial frequency and the needle spreads them across the scale by position.
//
// 2026-10-17 重新逐条实测(本机直连取流 5 秒, 解析 MPEG 帧同步字并核对帧长序列):
// ① 旧注释说 qtfm/qingting 的 64k.mp3 "返回随机字节" —— 那个结论已不成立。
//    现在这批流每条都能解出连续定长 MPEG 帧(~16 kB/s, 与 128kbps 一致),
//    各地城市台正是走这个源, 所以本地台列表可以依赖它。
// ② 表里凡是"中文台名 + 实际放英文氛围乐"的一律删掉(SomaFM 的 dronezone /
//    secretagent 曾被标成"Fred 电影电台""亚洲音乐台", 属于错标)。
// ③ FM891 的 96k.aac 与 qingting 的 kuwo "音乐之声" 变体都不是稳定直播流
//    (后者实测 238 kB/s 的整块下载, 是单曲而非电台), 同样删除。
// 2026-10-26 兜底表: 用户从 29 个实测全国台中点选 15 个, 显示名按用户指定。
// 全部 OK 级(投递余量 >= 1.1x 内容码率, 12s 采样无停顿), 高码率组优先。
const RadioStation kFallbackStations[] = {
    {"音乐之声", "全国",
     "http://antiserver.kuwo.cn/anti.s?rid=MUSIC_190038250&response=res&format=mp3|aac&type=convert_url&br=128kmp3&agent=iPhone&callback=getlink&jpcallback=getlink.mp3", 0},   // 128kbps 1330.0KB/s 余量0.01 (原名: CNR-3 音乐之声)
    {"亚洲音乐台", "全国",
     "https://antares.dribbcast.com/proxy/apop?mp=/s", 0},   // 128kbps 102.2KB/s 余量0.16 (原名: Big B Radio亚洲音乐台)
    {"5分钟新闻", "全国",
     "https://replaynewszh.ice.infomaniak.ch/replaynewszh-128.mp3", 0},   // 128kbps  27.3KB/s 余量0.59 (原名: REPLAY NEWS - 中文 每5分钟播报一次新)
    {"听云戏曲台", "全国",
     "https://m.tyradio.uk/tyxq.mp3", 0},   //  96kbps  16.7KB/s 余量0.72 (原名: 听云戏曲台)
    {"听云相声台", "全国",
     "https://m.tyradio.uk/tyxs.mp3", 0},   //  96kbps  16.5KB/s 余量0.73 (原名: 听云相声台)
    {"听云评书台", "全国",
     "https://m.tyradio.uk/typs.mp3", 0},   //  96kbps  16.2KB/s 余量0.74 (原名: 听云评书台)
    {"新闻联播", "全国",
     "https://rfienchinois64k.ice.infomaniak.ch//rfienchinois-64.mp3", 0},   //  64kbps  18.3KB/s 余量0.44 (原名: 新闻联播)
    {"中国之声", "全国",
     "https://lhttp.qtfm.cn/live/15318317/64k.mp3", 0},   //  64kbps  12.8KB/s 余量0.62 (原名: CNR-1 中国之声)
    {"国际新闻", "全国",
     "https://lhttp.qtfm.cn/live/20500172/64k.mp3", 0},   //  64kbps  12.8KB/s 余量0.63 (原名: 国际新闻)
    {"故事广播", "全国",
     "https://lhttp.qtfm.cn/live/20500182/64k.mp3", 0},   //  64kbps  12.7KB/s 余量0.63 (原名: 故事广播-阿基米德故事会)
    {"动听音乐台", "全国",
     "https://lhttp-hw.qtfm.cn/live/5022107/64k.mp3", 0},   //  64kbps  12.6KB/s 余量0.64 (原名: 动听音乐台)
    {"星空电台", "全国",
     "https://lhttp-hw.qtfm.cn/live/5022379/64k.mp3", 0},   //  64kbps  12.4KB/s 余量0.65 (原名: 星空电台 STAR RADIO)
    {"健康电台", "全国",
     "https://lhttp-hw.qtfm.cn/live/20500232/64k.mp3", 0},   //  64kbps  12.9KB/s 余量0.62 (原名: 阿基米德-健康电台)
    {"听梦想FM", "全国",
     "https://lhttp-hw.qtfm.cn/live/4917/64k.mp3", 0},   //  64kbps  12.8KB/s 余量0.62 (原名: .听梦想FM（原态度电台）24小时音乐不打烊)
    {"城市音乐台", "全国",
     "https://lhttp.qtfm.cn/live/20500153/64k.mp3", 0},   //  64kbps  12.7KB/s 余量0.63 (原名: CityFM城市音乐台)
};


// 城市预设表: 选城市时按组键动态拷入电台列表(不占兜底静态表)。
// 组键 = 省级(如 广东/江苏), 每组按实测余量比升序(最稳的在前)。
// 城市预设表: 选省份/城市时按组键动态拷入电台列表(不占兜底静态表)。
// 每组 <= 12 台(实测可持续), 重庆 5 台为 v1.2.14 专项补测。
const RadioStation kCityPresetStations[] = {
    {"Alxa Chinese Radio", "内蒙古",
     "http://lhttp.qingting.fm/live/5022521/64k.mp3", 0},   //  64kbps  12.8 余量0.63
    {"赤峰蒙语广播", "内蒙古",
     "https://lhttp-hw.qtfm.cn/live/1897/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"北京新闻广播", "北京",
     "https://lhttp.qtfm.cn/live/339/64k.mp3", 0},   //  64kbps  12.9 余量0.62
    {"南阳综合广播", "南阳",
     "https://lhttp.qingting.fm/live/1213/64k.mp3", 0},   //  64kbps  12.9 余量0.62
    {"MY FM全国音乐频道——FM106.3  长春少儿", "吉林",
     "http://lhttp.qingting.fm/live/4984/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"长春交通之声", "吉林",
     "http://lhttp.qingting.fm/live/4967/64k.mp3", 0},   //  64kbps  12.8 余量0.63
    {"四川新闻广播", "四川",
     "https://lhttp.qtfm.cn/live/4906/64k.mp3", 0},   //  64kbps  12.8 余量0.63
    {"四川音乐广播", "四川",
     "https://lhttp.qtfm.cn/live/1110/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"安徽交通广播", "安徽",
     "https://lhttp.qtfm.cn/live/1949/64k.mp3", 0},   //  64kbps  20.5 余量0.39
    {"济南都市广播", "山东",
     "https://lhttp.qtfm.cn/live/5022333/64k.mp3", 0},   //  64kbps  12.8 余量0.63
    {"聊城经济广播", "山东",
     "https://lhttp.qtfm.cn/live/5022262/64k.mp3", 0},   //  64kbps  12.6 余量0.64
    {"青岛新闻广播", "山东",
     "http://lhttp.qingting.fm/live/1673/64k.mp3", 0},   //  64kbps  12.3 余量0.65
    {"广州新闻资讯广播台", "广东",
     "http://lhttp.qingting.fm/live/4848/64k.mp3", 0},   //  64kbps  12.9 余量0.62
    {"深圳新闻广播", "广东",
     "http://lhttp.qingting.fm/live/1270/64k.mp3", 0},   //  64kbps  12.9 余量0.62
    {"深圳音乐广播", "广东",
     "https://lhttp.qtfm.cn/live/1271/64k.mp3", 0},   //  64kbps  12.9 余量0.62
    {"羊城交通电台", "广东",
     "https://lhttp.qtfm.cn/live/1262/64k.mp3", 0},   //  64kbps  12.8 余量0.62
    {"顺德音乐之声", "广东",
     "https://lhttp.qtfm.cn/live/20500150/64k.mp3", 0},   //  64kbps  13.0 余量0.62
    {"中山综合广播·新锐967", "广东",
     "https://lhttp.qingting.fm/live/1277/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"广东城市之声", "广东",
     "https://lhttp.qtfm.cn/live/469/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"广东新闻广播", "广东",
     "https://lhttp.qtfm.cn/live/1254/64k.mp3", 0},   //  64kbps  12.8 余量0.63
    {"广东珠江经济台", "广东",
     "https://lhttp.qtfm.cn/live/1259/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"广东音乐之声", "广东",
     "https://lhttp.qtfm.cn/live/1260/64k.mp3", 0},   //  64kbps  12.6 余量0.63
    {"广州汽车音乐电台金曲音乐广播", "广东",
     "http://lhttp.qingting.fm/live/20192/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"广州经济交通广播电台", "广东",
     "http://lhttp.qingting.fm/live/4955/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"广西教育广播 私家车930", "广西",
     "https://lhttp-hw.qtfm.cn/live/1756/64k.mp3", 0},   //  64kbps  13.2 余量0.61
    {"怀集音乐之声", "广西",
     "https://lhttp.qingting.fm/live/4804/64k.mp3", 0},   //  64kbps  12.6 余量0.64
    {"Changchi News Radio", "新疆",
     "http://lhttp.qingting.fm/live/5021874/64k.mp3", 0},   //  64kbps  12.9 余量0.62
    {"轮台之声", "新疆",
     "https://lhttp-hw.qtfm.cn/live/20500099/64k.mp3", 0},   //  64kbps  12.5 余量0.64
    {"无锡新闻综合广播FM93.7", "江苏",
     "https://lhttp.qtfm.cn/live/2777/64k.mp3", 0},   //  64kbps  12.9 余量0.62
    {"苏州都市音乐广播", "江苏",
     "http://lhttp.qingting.fm/live/2803/64k.mp3", 0},   //  64kbps  12.8 余量0.62
    {"三门峡交通文艺广播", "河南",
     "https://lhttp-hw.qtfm.cn/live/15318227/64k.mp3", 0},   //  64kbps  12.9 余量0.62
    {"杭州华语之声网络广播", "浙江",
     "https://lhttp.qtfm.cn/live/20505/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"海口综合广播", "海口",
     "https://lhttp.qtfm.cn/live/5022015/64k.mp3", 0},   //  64kbps  12.9 余量0.62
    {"FM104.7 澧县人民广播电台", "湖北",
     "https://lhttp-hw.qtfm.cn/live/15318178/64k.mp3", 0},   //  64kbps  13.2 余量0.60
    {"鼎城人民广播电台", "湖南",
     "https://lhttp.qingting.fm/live/5021860/64k.mp3", 0},   //  64kbps  12.5 余量0.64
    {"福建海峡之声", "福建",
     "https://lhttp-hw.qtfm.cn/live/1744/64k.mp3", 0},   //  64kbps  13.0 余量0.62
    {"厦门音乐广播", "福建",
     "https://lhttp.qtfm.cn/live/1739/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"西宁交通文艺广播", "西宁",
     "http://lhttp.qingting.fm/live/5022283/64k.mp3", 0},   //  64kbps  12.8 余量0.62
    {"大连音乐广播", "辽宁",
     "http://lhttp.qingting.fm/live/1084/64k.mp3", 0},   //  64kbps  13.0 余量0.62
    {"沈阳新闻广播", "辽宁",
     "http://lhttp.qingting.fm/live/20024/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"北碚综合广播·嘉陵之声FM88.7", "重庆",
     "https://lhttp-hw.qtfm.cn/live/20211692/64k.mp3", 0},   //  64kbps  12.8 余量0.62
    {"大足综合广播", "重庆",
     "https://lhttp-hw.qtfm.cn/live/20211676/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"綦江综合广播", "重庆",
     "http://lhttp.qtfm.cn/live/20500201/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"永川之声100.7", "重庆",
     "http://lhttp.qtfm.cn/live/20210236/64k.mp3", 0},   //  64kbps  12.4 余量0.64
    {"重庆都市广播", "重庆",
     "http://lhttp.qtfm.cn/live/1502/64k.mp3", 0},   //  64kbps  12.5 余量0.64
    {"长沙交通广播", "长沙",
     "http://lhttp.qingting.fm/live/3967/64k.mp3", 0},   //  64kbps  13.0 余量0.62
    {"长沙音乐广播", "长沙",
     "https://lhttp.qtfm.cn/live/20847/64k.mp3", 0},   //  64kbps  13.0 余量0.62
    {"陕西交通广播", "陕西",
     "https://lhttp.qtfm.cn/live/1601/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"陕西音乐广播", "陕西",
     "https://lhttp.qtfm.cn/live/4873/64k.mp3", 0},   //  64kbps  12.7 余量0.63
    {"青海交通音乐广播 FM97.2", "青海",
     "http://lhttp.qtfm.cn/live/5009/64k.mp3", 0},   //  64kbps  13.0 余量0.62
    {"青海经济生活广播 FM90.3", "青海",
     "http://lhttp.qtfm.cn/live/5008/64k.mp3", 0},   //  64kbps  12.8 余量0.63
};

// 组键 -> [起始, 数量] 索引(线性查找, 组数少)
struct CityPresetGroup {
    const char *group;
    uint16_t offset;
    uint16_t count;
};
const CityPresetGroup kCityPresetGroups[] = {
    {"内蒙古", 0, 2},
    {"北京", 2, 1},
    {"南阳", 3, 1},
    {"吉林", 4, 2},
    {"四川", 6, 2},
    {"安徽", 8, 1},
    {"山东", 9, 3},
    {"广东", 12, 12},
    {"广西", 24, 2},
    {"新疆", 26, 2},
    {"江苏", 28, 2},
    {"河南", 30, 1},
    {"浙江", 31, 1},
    {"海口", 32, 1},
    {"湖北", 33, 1},
    {"湖南", 34, 1},
    {"福建", 35, 2},
    {"西宁", 37, 1},
    {"辽宁", 38, 2},
    {"重庆", 40, 5},
    {"长沙", 45, 2},
    {"陕西", 47, 2},
    {"青海", 49, 2},
};

// 选城市时把预设组拷入 s_stations(见 radio_player_load_city_preset)。
std::atomic<bool> s_network_connected{false};
std::atomic<bool> s_wanted_playing{false};
std::atomic<std::size_t> s_station_index{0};
std::atomic<std::size_t> s_station_count{0};
static bool s_catalog_cached;
std::atomic<uint8_t> s_volume{55};
std::atomic<uint32_t> s_generation{1};
std::atomic<bool> s_stream_active{false};
// ui_sfx 播放提示音时置 true: 收音机**继续解码/缓冲**, 但暂时不写 I2S,
// 把音频设备让给提示音; 播完立即恢复。这样完成提醒不会被"正在播放"吞掉,
// 也不会像暂停那样丢掉正在缓冲的音频(等价于临时静音一小段)。
std::atomic<bool> s_audio_yield{false};
// Set when a UI effect temporarily reconfigures the codec. The stream loop
// must restore its own sample rate before writing another PCM frame; otherwise
// 48 kHz radio PCM is fed through a 16 kHz clock and sounds like hiss/distortion.
std::atomic<bool> s_audio_needs_reformat{false};
// 最近一次 stream_station 失败的原因是否为"内存不足"(而非电台本身坏)。
// 播放循环据此决定: 真坏台 -> 换台; 内存不足 -> 换台也没用, 用长退避等堆恢复。
std::atomic<bool> s_last_fail_lowmem{false};
// 本次 stream_station 是否处于"源在涓流喂食"状态(prebuffer 到期仍不足目标量)。
// 涓流台能解码出几十毫秒音频, 旧的 played_audio 判据把它当"活台"永远重连,
// 用户就卡在一个无声台里(2026-10-25 真机: 重庆都市广播整晚 12s 只给 ~9.6K)。
std::atomic<bool> s_last_prebuffer_starved{false};
// 池里的解码器是否已完成"首帧预热"。
std::atomic<bool> s_pool_warmed{false};
std::atomic<TaskHandle_t> s_player_task{nullptr};
SemaphoreHandle_t s_worker_mutex;
bool s_worker_quiescent;
SemaphoreHandle_t s_station_mutex;
SemaphoreHandle_t s_pool_mutex;
std::atomic<uint32_t> s_playback_revision{0};
RadioStation s_stations[kMaxStations] = {};

// The stream worker lives for the whole session (it is only suspended while
// paused). Like the bridge task, its 4.6KB stack was permanently taken from
// the heap; putting it in .bss leaves the allocator enough contiguous room for
// DNS/HTTP setup while the minimized radio and bridge run together on home.
static StaticTask_t s_player_task_tcb;
static StackType_t s_player_task_stack[4608];

// ---- MP3 解码器内存池(开机预分配) ----
// C3 上 WiFi 连上后堆碎片化(largest 仅 ~7.6KB), 现场 open 解码器(需 ~19KB
// 连续块)必然失败。故在启动早期(堆最大 largest≈114KB)一次性抢下解码器与
// 收发缓冲, 之后切台只 reset 复用, 不再反复分配。收音机是主功能, 值得优先。
esp_audio_simple_dec_handle_t s_decoder_pool = nullptr;
uint8_t *s_input_pool = nullptr;
uint8_t *s_output_pool = nullptr;
static std::size_t s_tail_arena_size = 0;

RadioStation station_snapshot(std::size_t index) {
    RadioStation station = {};
    if (!s_station_mutex ||
        xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return station;
    }
    const std::size_t count = std::max<std::size_t>(s_station_count.load(), 1);
    station = s_stations[index % count];
    xSemaphoreGive(s_station_mutex);
    return station;
}

void save_setting(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

uint8_t load_setting(const char *key, uint8_t fallback) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

void save_station_name(const char *name) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    if (name && name[0]) {
        nvs_set_str(handle, "station_name", name);
    } else {
        nvs_erase_key(handle, "station_name");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

// Copies the station name the listener last chose. Returns false when no
// station has been remembered yet.
bool load_station_name(char *name, std::size_t capacity) {
    if (!name || capacity == 0) return false;
    name[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t length = capacity;
    const esp_err_t error = nvs_get_str(handle, "station_name", name, &length);
    nvs_close(handle);
    if (error != ESP_OK) {
        name[0] = '\0';
        return false;
    }
    return name[0] != '\0';
}

// Installs a catalog under the station mutex and reports the index that should
// stay selected. `preferred_name` keeps the previous station when the fresh
// catalog still carries it.
bool install_stations(const RadioStation *stations, std::size_t count,
                      const char *preferred_name, std::size_t *selected) {
    if (!stations || count == 0 || !s_station_mutex) return false;
    count = std::min(count, kMaxStations);

    // Stop the current request before replacing its backing catalog. The
    // streaming task uses a value snapshot, so the swap is atomic to users.
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    std::memcpy(s_stations, stations, count * sizeof(RadioStation));
    if (count < kMaxStations) {
        std::memset(s_stations + count, 0,
                    (kMaxStations - count) * sizeof(RadioStation));
    }
    std::size_t index = 0;
    if (preferred_name && preferred_name[0]) {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(s_stations[i].name, preferred_name) == 0) {
                index = i;
                break;
            }
        }
    }
    s_station_count.store(count, std::memory_order_release);
    s_station_index.store(index, std::memory_order_release);
    xSemaphoreGive(s_station_mutex);
    if (selected) *selected = index;
    return true;
}

bool request_still_current(uint32_t generation, std::size_t station) {
    return s_network_connected.load(std::memory_order_acquire) &&
           s_wanted_playing.load(std::memory_order_acquire) &&
           s_generation.load(std::memory_order_acquire) == generation &&
           s_station_index.load(std::memory_order_acquire) == station;
}

// 可中断的延时。⚠ 失败退避最长会到 120 秒, 若用一次性 vTaskDelay, 用户在
// 退避期间按"暂停"再按"恢复"时, 播放任务仍在睡 -> 表现为"暂停后再也恢复不了"
// (实测用户反馈)。这里切成 100ms 一片, 每片检查"用户是否改过播放意图/换台",
// 一旦变化立即返回, 让外层循环立刻响应。
// 返回 true = 正常睡满; false = 被用户操作打断(应立刻重新处理)。
bool interruptible_delay_ms(uint32_t total_ms, uint32_t generation,
                            std::size_t station) {
    const uint32_t slice = 100;
    for (uint32_t waited = 0; waited < total_ms; waited += slice) {
        if (radio_retry_cancelled(
                s_wanted_playing.load(std::memory_order_acquire),
                s_network_connected.load(std::memory_order_acquire), generation,
                s_generation.load(std::memory_order_acquire), station,
                s_station_index.load(std::memory_order_acquire))) return false;
        const uint32_t remaining = total_ms - waited;
        vTaskDelay(pdMS_TO_TICKS(remaining < slice ? remaining : slice));
    }
    return true;
}
void calculate_levels(const uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || bytes < 64) return;
    const auto *samples = reinterpret_cast<const int16_t *>(pcm);
    const std::size_t sample_count = bytes / sizeof(int16_t);
    if (sample_count < kLevelCount) return;
    const std::size_t frames = sample_count / std::max<uint8_t>(channels, 1);
    if (frames < kLevelCount) return;

    uint8_t levels[kLevelCount] = {};
    for (std::size_t band = 0; band < kLevelCount; ++band) {
        const std::size_t begin = band * frames / kLevelCount;
        const std::size_t end = (band + 1) * frames / kLevelCount;
        int64_t energy = 0;
        std::size_t count = 0;
        for (std::size_t frame = begin; frame < end; ++frame) {
            int32_t mixed = 0;
            for (uint8_t channel = 0; channel < channels; ++channel) {
                mixed += samples[frame * channels + channel];
            }
            mixed /= std::max<uint8_t>(channels, 1);
            energy += static_cast<int64_t>(mixed) * mixed;
            ++count;
        }
        const float rms = count ? std::sqrt(static_cast<float>(energy) / count) : 0.0f;
        float db = rms > 1.0f ? 20.0f * std::log10(rms / 32768.0f) : -80.0f;
        int value = static_cast<int>((db + 58.0f) * 2.25f);
        value += static_cast<int>((band * 13 + s_generation.load()) % 9) - 4;
        levels[band] = static_cast<uint8_t>(std::clamp(value, 2, 100));
    }
    radio_ui_set_audio_levels(levels, kLevelCount);
}

// ---------------------------------------------------------------------------
// 流级上下文: 通过 HTTP 事件回调捕获响应头里的 icy-metaint。
// (esp_http_client_get_header 只能读**请求**头, 读不到响应头, 必须走事件。)
struct StreamContext {
    int icy_metaint = 0;
};

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    auto *ctx = static_cast<StreamContext *>(evt->user_data);
    if (!ctx || !evt->header_key) return ESP_OK;
    if (strcasecmp(evt->header_key, "icy-metaint") == 0 && evt->header_value) {
        ctx->icy_metaint = std::atoi(evt->header_value);
    }
    return ESP_OK;
}


// ---------------------------------------------------------------------------
// ICY (SHOUTcast) 元数据剥离。
//
// 【为什么必须有】多数网络电台(实测中国之声/中文新闻台/FM891/Fred/动漫/亚洲/
// 怀旧金曲 等 7 个中文台全部命中)在响应头返回 `icy-metaint: N`(8192~32768),
// 表示**每 N 字节音频后插入一个元数据块**: 1 字节长度 L(单位 16), 后跟 L*16
// 字节文本(如 `StreamTitle='歌名';`)。ESP-IDF 的 esp_http_client **完全不做**
// 这件事(全局无 metaint 代码)。
//
// 不做剥离的后果: 元数据字节被当 MP3 数据送解码器 -> 解码器输出噪声,
// 听觉表现为**周期性电流声/咔哒**, 且解码错误计数上升。这正是"音质差"的根因。
//
// 本结构体跨多次 read 累积状态, 精确切掉元数据块。
//
// ⚠ 状态机只有两个计数, 不再需要额外的"待读长度字节"标志:
//   meta_left  > 0 → 正在丢弃元数据字节
//   audio_left > 0 → 正在拷贝音频字节
//   两者都为 0     → 下一个字节是本段音频之后紧跟的**长度字节**
//   因为 audio_left 是持久 int, "长度字节跨 read 边界"天然被处理
//   (它出现在 audio_left 恰好归零后的下一次调用里)。
// 早期版本在读到非空元数据时忘记重置 audio_left, 导致 audio_left 停在 0,
// 之后每个字节都被当长度字节消费 -> 每 1 字节吞掉 16 字节音频(丢 20% 数据),
// 听觉表现正是"每 1~2 秒卡一下 + 电流声"。
struct IcyStripper {
    int metaint = 0;        // 0 = 该流无元数据(不剥离)
    int audio_left = 0;     // 当前音频段还剩多少字节(>0 = 在音频中)
    int meta_left = 0;      // 当前元数据块还剩多少字节(>0 = 在元数据中)

    void begin(int metaint_value) {
        if (metaint_value > 0) {
            metaint = metaint_value;
            audio_left = metaint;
        } else {
            metaint = 0;
        }
        meta_left = 0;
    }

    // 原地剥离(把音频前移), 返回纯音频字节数。跨调用保持状态。
    std::size_t process(uint8_t *buf, std::size_t len) {
        if (metaint <= 0) return len;
        std::size_t rd = 0, wr = 0;
        while (rd < len) {
            if (meta_left > 0) {              // ① 丢弃元数据字节
                const std::size_t take = std::min<std::size_t>(
                    static_cast<std::size_t>(meta_left), len - rd);
                rd += take;
                meta_left -= static_cast<int>(take);
                continue;
            }
            if (audio_left > 0) {             // ② 拷贝音频字节
                const std::size_t take = std::min<std::size_t>(
                    static_cast<std::size_t>(audio_left), len - rd);
                if (wr != rd) std::memmove(buf + wr, buf + rd, take);
                wr += take;
                rd += take;
                audio_left -= static_cast<int>(take);
                continue;
            }
            // ③ 音频段读完: 本字节是长度 L(单位 16), 之后跟 L*16 字节元数据
            const int l = buf[rd] * 16;
            ++rd;
            audio_left = metaint;             // ★ 关键: 无论 L 是否为 0 都要重置
            if (l > 0) meta_left = l;
        }
        return wr;
    }
};

// ---------------------------------------------------------------------------
std::size_t downmix_to_mono(uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~static_cast<std::size_t>(1);

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const std::size_t frames = bytes / (sizeof(int16_t) * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        int32_t mixed = 0;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            mixed += samples[frame * channels + channel];
        }
        samples[frame] = static_cast<int16_t>(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

esp_audio_simple_dec_handle_t open_mp3_decoder() {
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    const esp_audio_err_t error = esp_audio_simple_dec_open(&config, &decoder);
    if (error != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "MP3 decoder open failed: %d", error);
        return nullptr;
    }
    return decoder;
}

}  // namespace

// ---------------------------------------------------------------------------
// media_lib 分配器接管(尾部竞技场, 2026-10-25)。
// 反汇编实测: esp_audio_codec 预编译库(audio_codec_mem.c.obj)里的
// media_lib_module_calloc/malloc/realloc/free 全是 weak 符号, 默认实现就是
// calloc/malloc/realloc/free 直通。这里给出强符号(文件作用域, 保证符号外部
// 可见以覆盖 weak 默认), 把 MP3 解码器首帧初始化的一串 >=2KB 子块(实测
// 4224+2304+2312+8192..., 合计 ~26KB)中能装下的部分重定向到小智闲置 mic
// 静态区的"尾巴"(9.2KB, 平时借给收音机抖动环, 见 xiaozhi_client.c)。
// 尾部与桥接任务栈(前 12KB)不重叠 -> **桥接可以常驻首页**, 计数/提醒照常;
// 堆里因此多出 ~8.8KB, 首页播放时 WiFi 收包(需 ~1600B 连续 DMA)不再被挤死。
constexpr std::size_t kArenaMinBlock = 2048;
static uint8_t *s_tail_arena_base = nullptr;   // 竞技场基址(nullptr = 未接管)
static uint8_t *s_tail_arena_end = nullptr;
static uint8_t *s_tail_arena_bump = nullptr;   // nullptr = 未接管

extern "C" void *media_lib_module_calloc(uint32_t module, size_t n, size_t size) {
    (void)module;
    const size_t total = n * size;
    if (total >= kArenaMinBlock && s_tail_arena_bump != nullptr) {
        const size_t block = (total + 15u) & ~(size_t)15u;  // 16B 对齐
        uint8_t *slot = s_tail_arena_bump;
        // 只用尾巴的前 XZ_MIC_TAIL_ARENA_CAP 字节: 剩余的留给抖动环,
        // 环容量 11 -> 13 块(用户实测: 整段占用会让源抖动吸收能力不足)。
        if (slot + block <= s_tail_arena_base + XZ_MIC_TAIL_ARENA_CAP) {
            *(volatile uint32_t *)slot = (uint32_t)total;  // 块头: 原尺寸
            uint8_t *out = slot + 16;
            s_tail_arena_bump = slot + block + 16;         // 头随块 16B 对齐
            std::memset(out, 0, total);
            return out;
        }
        // 竞技场满了: 余下的子块回堆(不失败, 只少省一点)。
    }
    return calloc(n, size);
}

extern "C" void *media_lib_module_realloc(uint32_t module, void *ptr, size_t size) {
    (void)module;
    if (ptr >= s_tail_arena_base && ptr < s_tail_arena_end) {
        // pvmp3 不 realloc 解码块; 防御性兜底: 搬到堆, 避免对静态区指针
        // 直接 realloc 崩溃。
        const size_t old = (size_t)(*(volatile uint32_t *)((uint8_t *)ptr - 16));
        void *heap_ptr = malloc(size);
        if (heap_ptr != nullptr && old != 0) {
            std::memcpy(heap_ptr, ptr, size < old ? size : old);
        }
        return heap_ptr;
    }
    return realloc(ptr, size);
}

extern "C" void media_lib_free(void *ptr) {
    if (ptr == nullptr) return;
    if (ptr >= s_tail_arena_base && ptr < s_tail_arena_end) return;  // 整片随池释放
    free(ptr);
}

// 一段最小的**合法 MP3 帧**(静音), 用于逼解码器把首帧内存分配出来。
// 取一个标准 MPEG-1 Layer III 帧头(0xFF 0xFB 0x90 0x00 = 128kbps/44.1kHz/单声道)
// 加上足够的零填充 —— 解码器解析到帧头后会分配内部解析上下文与帧缓冲(约 20KB)。
// 这一步务必在**堆充裕时**做, 做完把句柄复用(切台只 reset), 之后永不再需要那 20KB。
static const uint8_t kSilentMp3Frame[417] = {
    0xFF, 0xFB, 0x90, 0x00,   // MPEG-1 Layer III, 128kbps, 44.1kHz, mono
    // 其余留 0(帧数据不重要, 解码器只需成功解析帧头并完成内部初始化)
};

// 让池里的解码器完成"首帧预热"——把内部大块内存提前分配出来。
//
// ✅ 实测有效(2026-10-10, 修正后): 喂一个**合法 MPEG-1 Layer III 帧**
// (0xFF 0xFB 0x90 0x00 = 128kbps/44.1kHz/mono, 与主流电台一致) 能让解码器
// 真正解码出一帧 PCM: 日志 `decoded=4608, warmed=1`。
//   - 输出缓冲必须 >= 4608B(满帧 1152 样本 x 2ch x 2B), 故 kOutputSize=5120。
//     (曾误设 4096 -> result=-8, decoded=0, 预热失败。)
//   - 预热在开机(heap largest=114688)时完成, 之后**不 reset、不释放池**,
//     内部缓冲常驻 -> 播放时无需再要 20KB 连续块。
// 因此 stream_station 在 warmed 时可跳过大块内存检查(见那里的说明)。
static bool warm_decoder_with_scratch(const char *who) {
    if (!s_decoder_pool) return false;
    const std::size_t need = kInputSize + kPcmCapacity;
    uint8_t *scratch = nullptr;
    bool scratch_from_heap = false;
    std::size_t idle_size = 0;
    uint8_t *idle = xiaozhi_idle_buffer(&idle_size);
    if (idle != nullptr && idle_size >= need) {
        scratch = idle;
    } else {
        scratch = static_cast<uint8_t *>(malloc(need));
        scratch_from_heap = true;
    }
    if (scratch == nullptr) {
        ESP_LOGW(kTag, "radio %s: no scratch for decoder warmup (largest=%u)",
                 who, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    // Signal path uses two separate buffers; keep them non-overlapping here too.
    uint8_t *scratch_in = scratch;
    uint8_t *scratch_out = scratch + kInputSize;
    std::memset(scratch_in, 0, kInputSize);
    std::memcpy(scratch_in, kSilentMp3Frame, sizeof(kSilentMp3Frame));
    esp_audio_simple_dec_raw_t raw = {
        .buffer = scratch_in,
        .len = static_cast<uint32_t>(sizeof(kSilentMp3Frame)),
        .eos = false,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };
    esp_audio_simple_dec_out_t frame = {
        .buffer = scratch_out,
        .len = static_cast<uint32_t>(kPcmCapacity),
        .needed_size = 0,
        .decoded_size = 0,
    };
    const esp_audio_err_t warm =
        esp_audio_simple_dec_process(s_decoder_pool, &raw, &frame);
    const bool warmed = frame.decoded_size > 0;
    s_pool_warmed.store(warmed, std::memory_order_release);
    ESP_LOGW(kTag,
             "radio %s: scratch warmup result=%d (decoded=%u, warmed=%d, "
             "largest=%u, scratch=%s)",
             who, (int)warm, (unsigned)frame.decoded_size, (int)warmed,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             scratch_from_heap ? "heap" : "xiaozhi-idle");
    if (scratch_from_heap) free(scratch);
    return warmed;
}

static void warm_pool_decoder(const char *who) {
    if (!s_decoder_pool || !s_input_pool || !s_output_pool) return;
    std::memset(s_input_pool, 0, kInputSize);
    std::memcpy(s_input_pool, kSilentMp3Frame, sizeof(kSilentMp3Frame));
    esp_audio_simple_dec_raw_t raw = {
        .buffer = s_input_pool,
        .len = static_cast<uint32_t>(sizeof(kSilentMp3Frame)),
        .eos = false,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };
    esp_audio_simple_dec_out_t frame = {
        .buffer = s_output_pool,
        .len = kPcmCapacity,
        .needed_size = 0,
        .decoded_size = 0,
    };
    const esp_audio_err_t warm =
        esp_audio_simple_dec_process(s_decoder_pool, &raw, &frame);
    // ⚠ 只有**真的解出数据**(decoded_size > 0)才算预热成功。
    // 之前写成 `warm == OK` 会把"帧头没识别(decoded=0)"也误判为成功。
    const bool warmed = frame.decoded_size > 0;
    s_pool_warmed.store(warmed, std::memory_order_release);
    ESP_LOGW(kTag,
             "radio %s: first-frame warmup result=%d (decoded=%u, warmed=%d, "
             "heap largest=%u)",
             who, (int)warm, (unsigned)frame.decoded_size, (int)warmed,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

bool radio_player_prewarm(void) {
    if (!s_pool_mutex) s_pool_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_pool_mutex) return false;
    if (s_decoder_pool) return true;
    // 解码器注册必须先于 open。radio_player_init 里也会注册(幂等)。
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (!xiaozhi_radio_tail_available()) {
        // 小智退出收尾窗口: 有界等尾部归还, 否则池建在堆上且整场播放不纠正。
        for (int waited = 0; waited < 30 && !xiaozhi_radio_tail_available();
             ++waited) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (xiaozhi_radio_tail_available()) {
        // 尾部竞技场可用: 池缓冲先分配, 预热用池内缓冲做 I/O(绝不能走
        // scratch 路径 —— 那会先借走尾部, 随后竞技场 bump 落在同一片区,
        // 实测预热 decoded=0, 2026-10-25)。
        uint8_t *tail = xiaozhi_radio_tail_claim(&s_tail_arena_size);
        if (tail != nullptr) {
            s_tail_arena_base = tail;
            s_tail_arena_end = tail + s_tail_arena_size;
            s_tail_arena_bump = tail;
        }
        s_output_pool = static_cast<uint8_t *>(malloc(kOutputSize));
        s_input_pool = static_cast<uint8_t *>(malloc(kInputSize));
        s_decoder_pool = open_mp3_decoder();
        if (s_decoder_pool) warm_pool_decoder("prewarm-tail");
        if (s_decoder_pool && !s_pool_warmed.load(std::memory_order_acquire)) {
            warm_decoder_with_scratch("prewarm-tail-fallback");
        }
    } else {
        s_decoder_pool = open_mp3_decoder();
        // ⚠⚠ 关键(2026-10-10 发热事故的根本修复, 2026-10-25 补顺序修复):
        // 解码器句柄打开 ≠ 首帧内存已就绪。真正的 22904B(0x5978) 连续块是在
        // **首次 process() 内部**懒分配的。竞技场不可用(小智占用 mic 区)时
        // 它来自堆: 如果先 malloc 池缓冲再预热, 会把唯一的大块切碎, 预热以
        // ret 10 失败。所以必须先用暂存区喂首帧拿到内部大块, 再分配池缓冲。
        warm_decoder_with_scratch("prewarm");
        s_output_pool = static_cast<uint8_t *>(malloc(kOutputSize));
        s_input_pool = static_cast<uint8_t *>(malloc(kInputSize));
    }

    const bool ok = s_decoder_pool && s_output_pool && s_input_pool;
    ESP_LOGW(kTag, "radio prewarm: decoder=%d in=%p out=%p (heap largest=%u)",
             s_decoder_pool != nullptr, (void *)s_input_pool, (void *)s_output_pool,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    // 即使部分失败也不回滚: 现场路径会在 !decoder 时报错并退避, 不至于更糟。
    return ok;
}

// 释放收音机内存池: 进入小智等需要大块连续内存的应用前调用(此时收音机已暂停)。
// 退出后再由 radio_player_ensure_pool() 重新抢占。
// 这是"无后台应用"内存模型的落地: 谁在用谁占内存, 不用即还。
bool radio_player_has_pool() {
    return s_decoder_pool != nullptr && s_output_pool != nullptr && s_input_pool != nullptr;
}

void radio_player_release_pool(void) {
    xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY);
    ESP_LOGW(kTag, "release begin: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (s_decoder_pool) {
        esp_audio_simple_dec_close(s_decoder_pool);
        s_decoder_pool = nullptr;
        ESP_LOGW(kTag, "release after decoder close: free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    if (s_output_pool) {
        free(s_output_pool);
        s_output_pool = nullptr;
        ESP_LOGW(kTag, "release after output free: free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    if (s_input_pool) {
        free(s_input_pool);
        s_input_pool = nullptr;
        ESP_LOGW(kTag, "release after input free: free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    s_tail_arena_base = s_tail_arena_end = s_tail_arena_bump = nullptr;
    // 尾部竞技场随解码池一起归还: mic 静态区整片让给小智的 mic 任务栈。
    xiaozhi_radio_tail_release();
    s_pool_warmed.store(false, std::memory_order_release);
    ESP_LOGW(kTag, "radio pool released by caller (heap largest=%u)",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    xSemaphoreGiveRecursive(s_pool_mutex);
}

// 重新抢占内存池(退出小智/回收音机时)。可能失败(碎片), 失败则收音机走
// 现场分配路径(大概率开不出解码器, 但不会崩)。
void radio_player_ensure_pool(void) {
    // Entering the radio UI must not wait for the lifetime of an active stream.
    if (s_stream_active.load(std::memory_order_acquire) &&
        s_pool_warmed.load(std::memory_order_acquire)) return;
    xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY);
    if (s_decoder_pool) {
        // 池在但可能没预热(例如上次预热失败) -> 补一次。
        if (!s_pool_warmed.load(std::memory_order_acquire)) {
            warm_pool_decoder("ensure");
            // A failed warmup can leave the MP3 decoder handle unusable
            // (ESP_MP3_DEC ret 10 on every later stream). Discard it and
            // rebuild once instead of carrying a poisoned handle forever.
            if (!s_pool_warmed.load(std::memory_order_acquire)) {
                ESP_LOGW(kTag, "radio pool warmup failed; rebuilding decoder");
                if (s_decoder_pool) {
                    esp_audio_simple_dec_close(s_decoder_pool);
                    s_decoder_pool = nullptr;
                }
                free(s_output_pool);
                s_output_pool = nullptr;
                free(s_input_pool);
                s_input_pool = nullptr;
            }
        }
        if (s_decoder_pool) {
            xSemaphoreGiveRecursive(s_pool_mutex);
            return;
        }
    }
    if (xiaozhi_radio_tail_available()) {
        // 尾部竞技场可用(小智关闭): 解码子块 >=2KB 的部分将落进 mic 静态区
        // 尾巴, 堆里少 ~8.8KB —— 桥接常驻的首页播放因此不再饿死 WiFi 收包。
        // 池缓冲先分配(堆顺序无所谓), 预热直接用池内缓冲做 I/O。
        uint8_t *tail = xiaozhi_radio_tail_claim(&s_tail_arena_size);
        if (tail != nullptr) {
            s_tail_arena_base = tail;
            s_tail_arena_end = tail + s_tail_arena_size;
            s_tail_arena_bump = tail;
        }
        s_output_pool = static_cast<uint8_t *>(malloc(kOutputSize));
        s_input_pool = static_cast<uint8_t *>(malloc(kInputSize));
        s_decoder_pool = open_mp3_decoder();
        if (s_decoder_pool) warm_pool_decoder("re-acquire-tail");
        if (s_decoder_pool && !s_pool_warmed.load(std::memory_order_acquire)) {
            warm_decoder_with_scratch("re-acquire-tail-fallback");
        }
    } else {
        s_decoder_pool = open_mp3_decoder();
        // 顺序同上(2026-10-25): 竞技场不可用时解码块仍来自堆, 必须先让解码器
        // 拿到 0x5978 连续块, 再分配池内缓冲。
        warm_decoder_with_scratch("re-acquire");
        s_output_pool = static_cast<uint8_t *>(malloc(kOutputSize));
        s_input_pool = static_cast<uint8_t *>(malloc(kInputSize));
    }
    // ⚠ 重建失败必须**彻底清干净**(2026-10-12): 否则会留下一个半死的
    //   s_decoder_pool 句柄 —— 后续 stream_station 看到它非空就直接复用, 于是
    //   永远用着一个没成功预热过的解码器, 表现为收音机一直卡在"正在缓冲"/
    //   "正在切换电台", 而且再也不会重试(池"看起来"是齐的)。
    if (!s_decoder_pool || !s_output_pool || !s_input_pool) {
        ESP_LOGE(kTag, "pool rebuild failed (decoder=%p out=%p in=%p); clearing",
                 (void *)s_decoder_pool, (void *)s_output_pool, (void *)s_input_pool);
        if (s_decoder_pool) { esp_audio_simple_dec_close(s_decoder_pool); s_decoder_pool = nullptr; }
        free(s_output_pool); s_output_pool = nullptr;
        free(s_input_pool);   s_input_pool = nullptr;
        xSemaphoreGiveRecursive(s_pool_mutex);
        return;
    }
    // 若暂存预热没有成功(极端碎片), 用池内缓冲再补一次; 此时至少句柄已就绪。
    if (!s_pool_warmed.load(std::memory_order_acquire)) {
        warm_pool_decoder("re-acquire-retry");
    }
    ESP_LOGW(kTag, "radio pool re-acquired: decoder=%d warmed=%d (largest=%u)",
             s_decoder_pool != nullptr,
             (int)s_pool_warmed.load(std::memory_order_acquire),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    xSemaphoreGiveRecursive(s_pool_mutex);
}

void radio_player_rewake_after_pool_ready(void) {
    // A canceled XiaoZhi task can briefly keep the decoder's large internal
    // block while the home layer is already visible. Warm the pool once more
    // after that task has returned Opus, then wake the player so a failed
    // first attempt does not leave it in the long low-memory backoff.
    radio_player_ensure_pool();
    if (!s_pool_warmed.load(std::memory_order_acquire)) {
        // The first warmup can leave the decoder handle in a failed state
        // (ESP_MP3_DEC ret 10). Keeping that handle makes every later stream
        // fail even after memory is available, so discard the whole pool and
        // try one clean rebuild now that Opus has actually been returned.
        radio_player_release_pool();
        vTaskDelay(pdMS_TO_TICKS(50));
        radio_player_ensure_pool();
    }
    if (s_player_task.load(std::memory_order_acquire) &&
        s_wanted_playing.load(std::memory_order_acquire) &&
        s_pool_warmed.load(std::memory_order_acquire)) {
        xTaskNotifyGive(s_player_task.load(std::memory_order_acquire));
    }
}

uint8_t *radio_player_search_buffer(void) {
    xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY);
    if (!s_stream_active.load(std::memory_order_acquire) && s_output_pool) return s_output_pool;
    xSemaphoreGiveRecursive(s_pool_mutex);
    return nullptr;
}
std::size_t radio_player_search_buffer_size(void) { return kOutputSize; }
void radio_player_release_search_buffer(void) {
    xSemaphoreGiveRecursive(s_pool_mutex);
}

namespace {

// 网络抖动缓冲上限。正常路径借用小智空闲的 25.6KB 麦克风栈，形成约
// 27KB 环形缓冲；heap 分配只作为 borrow 不可用时的兜底。
// 借用小智栈后的可用环: 25.6KB - 12KB(bridge) - 4KB(网络任务栈) ≈ 9.4KB,
// 即 9 个 1KB 块。heap 兜底分支仍按实际空闲堆尽量多分。
// 可用环 = 小智麦克风区余量 10KB + 小智 TTS 静态栈 10KB = 20 个 1KB 块。
// 两块内存都只在收音机运行时闲置, 且不进 .bss, 因此不会挤 WiFi 收包缓冲。
constexpr std::size_t kNetworkBlocks = 20;
// 抖动缓冲目标 12KB(20KB 环的 60%)。
//
// 2026-10-21 真机实测(北碚综合广播 / 中国之声, COM4):
//   切台后前两个 10s 窗口播放占比只有 2% 和 12%, 第三个窗口才回到 100%。
//   原因既不是内存也不是解码器, 而是 CDN 启动期只给 ~1KB/s(音频要 8KB/s),
//   每个窗口的 read_max 都打满 1001ms 超时。
//
//   旧配置(起播 3KB / 恢复上限 4KB)在慢源下的表现是:
//     播 0.5 秒 -> 停 4 秒 -> 又播 0.5 秒
//   因为 4KB 只等于 0.5 秒音频, 一攒满就被 8KB/s 的消费速度吃空。
//
//   改成 12KB(1.5 秒音频)后, 慢源期间是**一段安静等待**, 不再高频抽搐;
//   而且等待期间不消费, 环只进不出, 反而更快攒够 —— 等 CDN 爬坡结束,
//   环已接近满(实测窗口 6~8 稳定 20480 满环 + 100% 播放)。
//
//   代价: 切台首响变慢(慢源下最多等十几秒), 这是用户明确接受的取舍。
constexpr std::size_t kPrebufferTarget = 12 * 1024;
// 欠载后的恢复阈值上限, 与起播目标一致: 每次欠载都重新攒到 12KB 再出声。
// stream_station 还会把它夹到环容量的 3/4 以内, 保证任何时候都留 1/4 环
// 吸收突发(小环/堆兜底路径下也不会去等一个永远填不满的量而卡死)。
constexpr std::size_t kMaxResumePrebuffer = 12 * 1024;
// 预缓冲等待上限(2026-10-25 真机根因): 有些电台 CDN 会以极低速率持续滴流,
// 既不彻底静默(20s idle 看门狗不触发), 又永远攒不到 12KB —— 表现为界面
// 无限停在"正在缓冲"。到期后必须做决定: 手上的数据够就先播, 不够就放弃
// 本次尝试, 让外层按失败处理去换台。
constexpr std::uint32_t kPrebufferDeadlineMs = 12000;
// 到期时至少要有这么多音频才值得先播(约 0.5s @64kbps), 否则直接放弃。
constexpr std::size_t kPrebufferMinStartBytes = 4096;

// network_feed_task 的栈**静态分配**: 动态申请要求在播放时凑出一块连续堆,
// 而那时堆已被 LVGL/WiFi/音频分割到 largest<4KB, 必然失败
// (实测 "network task stack unavailable")。放进 .bss 就没有这个约束:
// 静态内存在开机就由 ELF 预留, 不参与堆碎片竞争。
//
// ⚠⚠ 2026-10-20 崩溃修复(真机复现: 设置 -> 城市电台 福建改重庆 -> 退出设置,
// 画面卡住一段时间后自己回到首页):
//   Guru Meditation Error: Core 0 panic'ed (Stack protection fault)
//   Detected in task "radio_network"; MEPC=_vfprintf_r, RA=esp_log
//   (调用链 esp_http_client_read -> lwip_select 的日志路径)
// 崩溃时 Stack bounds 为 2032B, SP 已越过下界约 320B —— 正好说明 2048B 不够。
// 本任务还持有 uint8_t bytes[kInputSize]=1024B 的局部数组, 切城市触发重连时
// 那条 lwIP 日志路径再要一块栈, 于是越界 -> 任务崩溃 -> RTC_SW_CPU_RST 重启。
// radio_network 的任务栈**不放在 .bss**, 而是从小智关闭时闲置的静态麦克风
// 区(residual 13KB)里划出 4KB, 见 xiaozhi_radio_stack()。原因(均为真机复现):
//   - 2048B 栈会在 esp_http_client_read -> lwIP/FreeRTOS 锁路径上踩穿栈保护,
//     任务 "radio_network" 每 ~20-40s 触发一次 RTC_SW_CPU_RST 自动重启;
//   - 把栈加到 2560/3072B(或把电台表扩回 14 项)会让 .bss 增长 512B~1KB,
//     直接挤掉 C3 的 WiFi 收包缓冲, 串口刷 "wifi:mem fail", TCP 吞吐从
//     11.7KB/s 掉到 300B/s, 表现就是收音机不停换台、没声音。
// 从小智静态区借用则两个问题都不存在, .bss 反而比原来更小。
// 读缓冲: 任务改用借用栈后 C3 栈预算充裕, 无需再用 256B 妥协。
constexpr std::size_t kNetworkFeedReadSize = 1024;
// 网络任务栈大小。实际存储由 xiaozhi_radio_stack() 从小智静态区提供;
// 4KB 是实测能覆盖 esp_http_client_read -> lwIP/FreeRTOS/newlib 锁路径的值。
constexpr std::size_t kNetworkFeedStack = 4096;

extern "C" uint8_t *xiaozhi_radio_stack(std::size_t *size);
extern "C" uint8_t *xiaozhi_tts_idle_buffer(std::size_t *size);
StaticTask_t s_network_feed_tcb;


struct NetworkFeed {
    esp_http_client_handle_t client;
    RadioStreamBuffer *ring;
    uint32_t generation;
    std::size_t station;
    std::atomic<bool> finished{false};
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> max_read_us{0};
    // 诊断: 本轮窗口内成功读到的字节数 / EAGAIN 次数 / 零读次数。
    std::atomic<uint32_t> diag_bytes{0};
    std::atomic<uint32_t> diag_eagain{0};
    std::atomic<uint32_t> diag_zero{0};
    // 0 = running, 1 = idle timeout, 2 = read error, 3 = EOF.
    // The feed task never logs itself; the player task (4.6KB stack) prints.
    std::atomic<uint8_t> exit_reason{0};
};

// 2026-10-17: 把"空读次数"改成"空读时长预算"。
// esp_http_client_read 的超时是 1000ms, 一次空读(EAGAIN)本身就阻塞满 1s,
// 原来的 empty_reads>=5 等于**任何停顿超过 5s 的直播源都被判死并拆流**。
// 中央台系(qtfm/qingting CDN)实测就是每 2s 突发一段数据, 首包之后还会静默
// 6~10s, 于是真机看到的现象是: 播放 1s -> 卡住 -> 每 10s 重连一轮
// (boot2.log: pcm=1123ms / wall=10223ms, gap_max=5198ms, queued=0),
// 用户听到的就是"说一半就不说了 / 一卡一卡"。
// 现在给 20s 的连续空读预算: 只要 20s 内回到数据就不重连。暂停/换台延迟
// 不受影响 —— stop 与 generation 检查仍在每轮循环最前面, 单轮最多阻塞一个
// 读超时(1s)。
constexpr int64_t kStallBudgetUs = 20 * 1000 * 1000;
// received==0 在 esp_http_client_read 里表示对端关闭(立即返回, 不阻塞),
// 这类失败要快速退出, 否则坏台会让用户等满整个停顿预算。
constexpr unsigned kEofReads = 3;

void network_feed_task(void *argument) {
    auto *feed = static_cast<NetworkFeed *>(argument);
    // 预留任务栈，等 HTTP 和环形缓冲就绪后才开始读取。
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t bytes[kNetworkFeedReadSize];
    unsigned eof_reads = 0;
    int64_t stall_start_us = 0;
    while (!feed->stop.load() && request_still_current(feed->generation, feed->station)) {
        const int64_t start = esp_timer_get_time();
        const int received = esp_http_client_read(feed->client,
                                                  reinterpret_cast<char *>(bytes),
                                                  sizeof(bytes));
        const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
        uint32_t maximum = feed->max_read_us.load();
        while (maximum < elapsed &&
               !feed->max_read_us.compare_exchange_weak(maximum, elapsed)) {}
        // ⚠ 本任务禁止调用 ESP_LOG*/printf: 那条 esp_log -> vfprintf 调用链
        // 需要在 2048B 栈上再要 1KB+, 2026-10-20 真机就是在这里踩穿栈保护。
        // 所有失败原因只写原子标志, 由 player_task 打印。
        if (received == -ESP_ERR_HTTP_EAGAIN) {
            feed->diag_eagain.fetch_add(1U);
            if (!stall_start_us) stall_start_us = start;
            if (esp_timer_get_time() - stall_start_us >= kStallBudgetUs) {
                feed->exit_reason.store(1U, std::memory_order_release);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (received < 0) {
            feed->exit_reason.store(2U, std::memory_order_release);
            break;
        }
        if (!received) {
            feed->diag_zero.fetch_add(1U);
            if (esp_http_client_is_complete_data_received(feed->client) ||
                ++eof_reads >= kEofReads) {
                feed->exit_reason.store(3U, std::memory_order_release);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        eof_reads = 0;
        stall_start_us = 0;
        feed->diag_bytes.fetch_add((uint32_t)received);
        std::size_t used = 0;
        while (used < static_cast<std::size_t>(received) &&
               !feed->stop.load() &&
               request_still_current(feed->generation, feed->station)) {
            used += feed->ring->write(bytes + used, received - used);
            if (used < static_cast<std::size_t>(received)) vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    // The owner deletes this worker before freeing its HTTP client or ring.
    feed->finished.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
}

bool stream_station(std::size_t station, uint32_t generation) {
    const RadioStation preset = station_snapshot(station);
    if (!preset.url[0]) {
        ESP_LOGE(kTag, "Station %u has no stream URL", static_cast<unsigned>(station));
        return false;
    }
    radio_ui_set_playback(RadioPlaybackState::Connecting, "正在连接电台");

    esp_http_client_config_t config = {};
    config.url = preset.url;
    // esp_http_client timeout_ms covers connect + first-header wait. 1.5s was
    // too aggressive for a cold DNS/TLS/ICY handshake and caused healthy
    // stations to report "HTTP status -1" exactly when the user switched:
    // they then waited through the outer retry backoff ("switch takes ages").
    // Give the connect phase a realistic budget here; after the headers arrive
    // the code below tightens the live read timeout to 1000ms so pause/switch
    // still cancels quickly.
    // 3.5s: enough for DNS + TCP + ICY headers on a healthy stream, but not so
    // long that a dead directory entry feels like a frozen dial. Live reads are
    // re-armed to 1000ms after the headers arrive (see below).
    config.timeout_ms = 3500;
    // ⚠ 2026-10-25 实测回退 2048 -> 1024:
    //   把接收缓冲提到 2048 后, 长时间运行(堆只剩 free≈5.5KB / largest≈2.4KB)
    //   时 2048 + socket 缓冲放不下, esp_http_client_open 直接失败:
    //     transport_base: Failed to open a new connection: 32769
    //     tcp close: fd=-1 ... errno=119
    //   结果是"一直在正在连接电台 -> 连不上 -> 切下一个台"地循环换台。
    //   1024 是能在这种紧堆状态下稳定建链的上限, 吞吐只能靠其它手段优化。
    //   (2026-10-13 的原始结论依然成立, 不要仅因为播放页 DMA 充裕就调大。)
    config.buffer_size = 1024;
    config.buffer_size_tx = 256;
    config.user_agent = "AI-Passport-Radio/1.1.1";
    config.keep_alive_enable = true;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 4;
    // 捕获响应头里的 icy-metaint(必须走事件回调, get_header 读不到响应头)
    StreamContext stream_ctx;
    config.event_handler = http_event_handler;
    config.user_data = &stream_ctx;
    if (std::strncmp(preset.url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "HTTP client allocation failed");
        s_last_fail_lowmem.store(true, std::memory_order_release);
        return false;
    }
    const auto close_client = [client]() {
        // This is a one-shot live stream. When the user changes station, wait
        // no longer applies and a graceful FIN leaves the old receive buffers
        // in TIME_WAIT for CONFIG_LWIP_TCP_MSL (~60s). Rapid switching then
        // exhausts DMA-capable heap and DNS starts returning getaddrinfo()=202.
        // Force an abortive close for this deliberately canceled stream so the
        // socket/PCB and its buffers are reclaimed immediately.
        const std::size_t free_before =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const std::size_t dma_before =
            heap_caps_get_free_size(MALLOC_CAP_DMA);
        const int fd = esp_http_client_get_socket(client);
        // lwIP only turns SO_LINGER=0 into an immediate RST when unsent or
        // unacknowledged data exists. A slow station may have no data queued,
        // in which case close() still sends FIN and leaves TIME_WAIT behind.
        // Queue one harmless byte after enabling linger: close() then takes
        // the abort path deterministically and reclaims the PCB immediately.
        int linger_result = -1;
        int abort_tx = -1;
        if (fd >= 0) {
            struct linger linger = {};
            linger.l_onoff = 1;
            linger.l_linger = 0;
            linger_result = setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger,
                                       sizeof(linger));
            if (linger_result == 0) {
                const char abort_byte = 0;
                abort_tx = send(fd, &abort_byte, sizeof(abort_byte), MSG_DONTWAIT);
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGW(kTag,
                 "tcp close: fd=%d abort_tx=%d linger=%d errno=%d free=%u->%u dma=%u->%u",
                 fd, abort_tx, linger_result, linger_result ? errno : 0,
                 (unsigned)free_before,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)dma_before,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    };
    // 请求 ICY 元数据: 必须发 1, 服务器才会在响应头返回 icy-metaint,
    // 我们据此剥离插入的元数据块(见 IcyStripper)。
    // ⚠ 发 0 时服务器**依然**周期性插入元数据, 只是不告诉我们间隔 -> 无解,
    // 只能盲切; 发 1 拿到 metaint 才能精确剥离。
    esp_http_client_set_header(client, "Icy-MetaData", "1");
    esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");

    bool played_audio = false;
    // 台连上了就先假设不饿; prebuffer 到期不足目标量时上面会置 true。
    s_last_prebuffer_starved.store(false, std::memory_order_release);
    esp_audio_simple_dec_handle_t decoder = nullptr;
    uint8_t *input = nullptr;
    uint8_t *output = nullptr;
    uint8_t *network_blocks[kNetworkBlocks] = {};
    std::size_t block_count = kNetworkBlocks;
    std::size_t prebuffer_bytes = 0;
    std::optional<RadioStreamBuffer> ring_storage;
    NetworkFeed feed{client, nullptr, generation, station};
    TaskHandle_t network_task = nullptr;

    // 复用开机预分配的解码器与缓冲(见 radio_player_prewarm): 切台时只 reset,
    // 不再 close/open。C3 上 WiFi 连上后堆碎片化(largest 仅 ~7.6KB), 现场
    // open 解码器(需 ~19KB 连续块)必然失败 -> 收音机无声的根因。
    //
    // ⚠ 兜底: 池可能被"进小智"或"城市搜索"释放过。这里再试一次恢复,
    // 因为此刻播放任务未运行、堆最干净; 若仍失败再退回现场分配。
    // 不这样做的话, 用户在池缺失时"点继续"会反复 open 失败 ->
    // 永远卡在"正在缓冲"(实测 ret 10 刷屏)。
    if (!s_decoder_pool) {
        ESP_LOGW(kTag, "decoder pool missing, attempting re-acquire (largest=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        radio_player_ensure_pool();
    }
    xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY);
    if (s_decoder_pool) {
        decoder = s_decoder_pool;
        input = s_input_pool;
        output = s_output_pool;
        // ⚠ 不再调用 esp_audio_simple_dec_reset(2026-10-10 实测):
        // 该函数的语义是"把 internal parser 和 decoder 都重置到初始态" ——
        // 实测它会把开机预热时分配好的那 ~20KB 内部缓冲**释放掉**, 导致
        // 播放时又需要重新分配 20480 连续块, 而此时堆已被 HTTP/WiFi 打碎, 拿不到
        // -> 收音机播不了。
        // 我们每次都是"切台即重新开始解码一条全新的流", 而 reset 的目的是复用
        // 句柄避免 close/open 开销; 既然它反而破坏内存, 就不调用它。
        // (非帧模式下从流头开始解码, 与 reset 后的行为一致。)
    } else {
        decoder = open_mp3_decoder();
        input = static_cast<uint8_t *>(malloc(kInputSize));
        output = static_cast<uint8_t *>(malloc(kOutputSize));
    }
    if (!input || !output) {
        ESP_LOGE(kTag, "stream buffers unavailable");
        close_client();
        xSemaphoreGiveRecursive(s_pool_mutex);
        return false;
    }

    // XiaoZhi and radio never own their task stacks at the same time. While
    // XiaoZhi is closed, its 25.6KB mic stack is dead RAM and makes an ideal
    // jitter pool: no heap allocation, no fragmentation, ~3s at 64kbps.
    uint8_t *borrowed_network = nullptr;
    uint8_t *borrowed_tts = nullptr;
    std::size_t borrowed_blocks = 0;
    std::size_t borrowed_mic_blocks = 0;
    std::size_t heap_network_start = kSharedNetworkBlocks;
    {
        std::size_t borrowed_size = 0;
        borrowed_network = xiaozhi_idle_buffer(&borrowed_size);
        if (borrowed_network && borrowed_size >= kInputSize) {
            borrowed_mic_blocks = std::min(borrowed_size / kInputSize,
                                           kNetworkBlocks - kSharedNetworkBlocks);
            borrowed_blocks = borrowed_mic_blocks;
        } else {
            borrowed_network = nullptr;
        }
        // TTS 静态栈同样只在收音机运行时闲置, 追加到同一环形缓冲后面。
        std::size_t tts_size = 0;
        borrowed_tts = xiaozhi_tts_idle_buffer(&tts_size);
        if (borrowed_tts != nullptr && tts_size >= kInputSize) {
            borrowed_blocks += std::min(tts_size / kInputSize,
                                        kNetworkBlocks - kSharedNetworkBlocks -
                                            borrowed_blocks);
        } else {
            borrowed_tts = nullptr;
        }
    }

        do {
        if (borrowed_blocks > 0) {
            block_count = kSharedNetworkBlocks + borrowed_blocks;
            prebuffer_bytes = std::min(
                (block_count - kSharedNetworkBlocks) * kInputSize,
                kPrebufferTarget);
            ring_storage.emplace(network_blocks, block_count, kInputSize);
            for (std::size_t i = 0; i < kSharedNetworkBlocks; ++i) {
                network_blocks[i] = output + kPcmCapacity + i * kInputSize;
            }
            // 先铺满小智麦克风区余量, 再用 TTS 静态栈续上。
            for (std::size_t i = 0; i < borrowed_blocks; ++i) {
                network_blocks[kSharedNetworkBlocks + i] =
                    i < borrowed_mic_blocks
                        ? borrowed_network + i * kInputSize
                        : borrowed_tts + (i - borrowed_mic_blocks) * kInputSize;
            }
            // 2026-10-25: 64k 台的缓冲余量为零 —— 13KB 环只够 1.6s, 而 CDN
            // 的抖动实测 gap 2.2~2.3s(用户: 一阵一阵的卡)。 borrowed 块铺完
            // 后用堆把环尽量补大 —— 但**必须带内存安全线**: 实测无脑补满 20
            // 块会把首页播放的堆吃到 free=5.4KB / dma=828B, WiFi 收包(1.6KB)
            // 分配不出来 -> 流饿死 -> 切台风暴 -> 进收音机建 UI 直接冻结。
            // 安全线: 内部堆空闲 >20KB 且最大连续块 >4KB(WiFi 收包 1.6KB 的
            // 2.5 倍)才继续补; 达线即停, 环有多少用多少。
            while (block_count < kNetworkBlocks &&
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 20000 &&
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) > 4096) {
                uint8_t *heap_block =
                    static_cast<uint8_t *>(malloc(kInputSize));
                if (heap_block == nullptr) break;
                network_blocks[block_count++] = heap_block;
            }
            prebuffer_bytes = std::min(
                (block_count - kSharedNetworkBlocks) * kInputSize,
                kPrebufferTarget);
            heap_network_start = block_count;
            ESP_LOGI(kTag,
                     "network buffer: blocks=%u capacity=%u prebuffer=%u source=idle-xiaozhi-stack+heap",
                     (unsigned)block_count,
                     (unsigned)(block_count * kInputSize),
                     (unsigned)prebuffer_bytes);
        }
        // 栈与 TCB 都在 .bss(见上方 kNetworkFeedStack), 不吃堆。
        // 优先级 7 > 解码任务的 5: TCP 接收窗口只有几 KB, 喂流任务必须在
        // socket 刚有数据时就排空, 否则 44.1kHz 立体声解码把 CPU 占满时
        // 窗口排不空 -> 源端停发 -> 缓冲见底 -> 周期性卡顿。仍低于
        // WiFi(23)/tcpip(18), 不会饿死网络栈。
        std::size_t radio_stack_size = 0U;
        uint8_t *radio_stack = xiaozhi_radio_stack(&radio_stack_size);
        if (radio_stack == nullptr || radio_stack_size < kNetworkFeedStack) {
            s_last_fail_lowmem.store(true, std::memory_order_release);
            ESP_LOGW(kTag, "radio network stack unavailable (size=%u)",
                     (unsigned)radio_stack_size);
            break;
        }
        network_task = xTaskCreateStatic(network_feed_task, "radio_network",
                                       kNetworkFeedStack, &feed, 7,
                                       (StackType_t *)radio_stack, &s_network_feed_tcb);
        if (network_task == nullptr) {
            s_last_fail_lowmem.store(true, std::memory_order_release);
            ESP_LOGW(kTag, "network feed task create failed");
            break;
        }
        if (esp_http_client_open(client, 0) != ESP_OK) {
            // Distinguish "no memory to build the connection" from "this
            // station is dead": only the latter should advance the dial.
            if (esp_http_client_get_errno(client) == ENOMEM || connect_out_of_memory()) {
                ESP_LOGW(kTag, "Open failed (low memory, largest=%u): %s",
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         preset.url);
                s_last_fail_lowmem.store(true, std::memory_order_release);
            } else {
                ESP_LOGW(kTag, "Open failed: %s", preset.url);
            }
            break;
        }
        // 内存探针(诊断): open 之后(socket/TCP 建立)的连续块大小。
        ESP_LOGW(kTag, "MEM after http_open: largest=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            // status -1 means fetch_headers failed outright; when that is
            // caused by an allocation failure it must not burn the dial.
            if (status < 0 && connect_out_of_memory()) {
                ESP_LOGW(kTag, "HTTP status %d (low memory, largest=%u)", status,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                s_last_fail_lowmem.store(true, std::memory_order_release);
            } else {
                ESP_LOGW(kTag, "HTTP status %d", status);
            }
            break;
        }

        if (!decoder) {
            // 解码器开不出来通常是内存不足(如 xiaozhi opus 预热后)。立即放弃
            // 本流并冷却, 否则外层立即重试会以毫秒级频率狂刷 MP3 init 失败,
            // CPU 满载烤机(实测 13:12 版固件发热根因)。
            ESP_LOGE(kTag, "MP3 decoder unavailable (heap?), backing off 10s");
            s_last_fail_lowmem.store(true, std::memory_order_release);
            interruptible_delay_ms(10000, generation, station);
            break;
        }
        if (!ring_storage.has_value()) {
            bool blocks_ready = true;
            // 给页面重绘留下 2KB。这个分支只在小智栈暂不可借时使用。
            const std::size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            constexpr std::size_t kUiReserve = 2048;
            const std::size_t affordable = free_bytes > kUiReserve
                ? kSharedNetworkBlocks + (free_bytes - kUiReserve) / kInputSize
                : kSharedNetworkBlocks;
            block_count = std::min(block_count, affordable);
            if (block_count < kSharedNetworkBlocks) {
                s_last_fail_lowmem.store(true, std::memory_order_release);
                ESP_LOGW(kTag, "insufficient heap for network buffer and UI reserve");
                break;
            }
            prebuffer_bytes = std::min((block_count - kSharedNetworkBlocks) * kInputSize,
                                       kPrebufferTarget);
            ESP_LOGI(kTag,
                     "network buffer: blocks=%u capacity=%u prebuffer=%u free=%u largest=%u source=heap",
                     (unsigned)block_count,
                     (unsigned)(block_count * kInputSize),
                     (unsigned)prebuffer_bytes,
                     (unsigned)free_bytes,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            ring_storage.emplace(network_blocks, block_count, kInputSize);
            for (std::size_t i = 0; i < block_count; ++i) {
                network_blocks[i] = i < kSharedNetworkBlocks
                    ? output + kPcmCapacity + i * kInputSize
                    : static_cast<uint8_t *>(malloc(kInputSize));
                if (!network_blocks[i]) { blocks_ready = false; break; }
            }
            if (!blocks_ready) {
                s_last_fail_lowmem.store(true, std::memory_order_release);
                ESP_LOGW(kTag, "network buffer unavailable");
                break;
            }
        }
        // 缓冲已就位: 借用静态栈路径与 heap 兜底路径共用同一份环形缓冲访问。
        RadioStreamBuffer &ring = *ring_storage;
        feed.ring = &ring;
        // Bound pause/cancel latency after the connection is established.
        // 2026-10-26: 1000ms -> 200ms。每轮 read 最多阻塞 1s 意味着环形缓冲
        // 快见底时, 一次 EAGAIN 就烧掉 1s —— 而 64k 台的抖动间隔实测 2~3s,
        // 两次 EAGAIN 直接把环抽干(13KB 环 = 1.6s)。200ms 的情况下同样的
        // 抖动只烧 0.2-0.4s, 环能扛过去。换台/暂停延迟不受影响: stop 与
        // generation 检查在每轮循环最前, 单轮阻塞最多 200ms。
        esp_http_client_set_timeout_ms(client, 200);
        xTaskNotifyGive(network_task);
        // 缓冲已在上面就位(池化复用或现场分配), 此处不再 malloc。

        radio_ui_set_playback(RadioPlaybackState::Buffering, "正在缓冲");
        bool format_ready = false;
        uint8_t source_channels = 1;
        bool buffering = true;
        int64_t buffering_since_us = esp_timer_get_time();
        // 恢复阈值上限 = 环容量的 3/4, 且不超过 kMaxResumePrebuffer(12KB)。
        // 先算上限、再夹起播阈值 —— 否则堆兜底路径下环可能只有 1~2KB,
        // 却去等 12KB, 永远等不满(表现为一直停在"正在缓冲")。
        const std::size_t wait_limit = std::min(
            kMaxResumePrebuffer, (block_count * kInputSize) * 3 / 4);
        // 本次等待的预缓冲阈值: 慢源从 12KB 起, 每次欠载仍可翻倍(但已顶到上限)。
        std::size_t wait_bytes = std::min(
            prebuffer_bytes ? prebuffer_bytes : kPrebufferTarget, wait_limit);
        bool playback_announced = false;
        uint32_t rebuffers = 0;
        int decode_fail_streak = 0;   // 连续解码失败计数(内存不足/坏流): 达限放弃本电台, 防3ms风暴
        bool stream_checked = false;  // 首块是否已做 MP3 同步字校验
        // 2026-10-17: 电流声/爆音根因。接入直播流是从**帧中间**开始的, 解码器
        // 在真正锁定位流之前解出的头一帧是垃圾(实测同一窗口 clipped=842
        // peak=32768, 也就是整段满幅方波直接推到 I2S -> 用户听到的"滋"一声)。
        // 欠载重新攒缓冲后同理。故这里丢掉接流后的前两帧 PCM(约 50ms),
        // 让耳朵只听到内容而不是对齐噪声。
        int join_warmup_frames = 2;
        int64_t stats_start = esp_timer_get_time();
        int64_t last_write_end = 0;
        uint32_t max_read_us = 0, max_gap_us = 0, slow_gaps = 0;
        uint32_t pcm_samples = 0, pcm_clipped = 0, sample_rate = 0;
        // Signal diagnostics: sum absolute 16-bit samples so we can tell
        // normal programme material apart from a low-level codec/PA floor.
        uint64_t pcm_abs_sum = 0;
        uint16_t pcm_peak = 0;
        uint8_t applied_volume = 0;
        IcyStripper icy;              // ICY 元数据剥离器(见 struct 定义)
        {
            // metaint 由 HTTP 事件回调在收响应头时写入 stream_ctx。
            // 无该头(如 SomaFM) -> metaint=0, process() 退化为直通。
            icy.begin(stream_ctx.icy_metaint);
            if (stream_ctx.icy_metaint > 0) {
                ESP_LOGI(kTag, "ICY metadata present: metaint=%d, will strip",
                         stream_ctx.icy_metaint);
            }
        }
        while (request_still_current(generation, station)) {
            const std::size_t available = ring.available();
            if (buffering) {
                const std::uint32_t waited_ms = static_cast<std::uint32_t>(
                    (esp_timer_get_time() - buffering_since_us) / 1000);
                const bool feed_done = feed.finished.load(std::memory_order_acquire);
                if (!feed_done && available < wait_bytes &&
                    waited_ms < kPrebufferDeadlineMs) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                if (available < wait_bytes) {
                    // 到期或源已结束: 不够最低可播量就放弃本次尝试(外层换台),
                    // 够就先播, 绝不允许无限停在"正在缓冲"。
                    if (feed_done || available < kPrebufferMinStartBytes) {
                        ESP_LOGW(kTag,
                                 "prebuffer stalled: %u/%u bytes after %ums; abandon attempt",
                                 (unsigned)available, (unsigned)wait_bytes,
                                 (unsigned)waited_ms);
                        s_last_prebuffer_starved.store(true, std::memory_order_release);
                        break;
                    }
                    ESP_LOGW(kTag,
                             "prebuffer deadline: start with %u/%u bytes after %ums",
                             (unsigned)available, (unsigned)wait_bytes,
                             (unsigned)waited_ms);
                    // 部分量开播 = 源在涓流喂食, 记 starvation 给外层跳台判据。
                    s_last_prebuffer_starved.store(true, std::memory_order_release);
                }
            }
            buffering = false;
            const int received = ring.read(input, kInputSize);
            if (!received) {
                if (feed.finished.load(std::memory_order_acquire)) break;
                buffering = true;
                buffering_since_us = esp_timer_get_time();
                join_warmup_frames = 2;   // 重新接入同样先丢两帧防爆音
                // 源端刚才停顿到把缓冲吃空: 下次多攒一点再出声, 避免"刚恢复
                // 又断"的连环卡顿。上限 kMaxResumePrebuffer。
                wait_bytes = std::min<std::size_t>(wait_bytes * 2, wait_limit);
                playback_announced = false;
                ++rebuffers;
                ESP_LOGW(kTag, "rebuffer #%u: next resume waits %u bytes",
                         (unsigned)rebuffers, (unsigned)wait_bytes);
                if (rebuffers >= 2) {
                    // 源反复断流(2026-10-25 实测涓流台每 10s 窗口恰好 2 次
                    // rebuffer, 统计窗口清零导致计数永远到不了 3): 与其让
                    // 用户听 30-60s 的卡顿+静音再等两轮饥饿会话, 不如放弃
                    // 本会话, 外层按饥饿计数立即跳台(冷却会让该台稍后轮回,
                    // CDN 恢复后还能听)。
                    //
                    // ⚠ 前提: rebuffer 只会在"欠载"时发生。起播阈值 12KB
                    // (=1.5s@64kbps)本身不欠载; 若连起播都进不来大量数据
                    // (12s 只到 4-10KB), 那是源极慢, 跳台同样正确。
                    ESP_LOGW(kTag, "session starved (%u rebuffers); abandoning",
                             (unsigned)rebuffers);
                    s_last_prebuffer_starved.store(true, std::memory_order_release);
                    break;
                }
                radio_ui_set_playback(RadioPlaybackState::Buffering, "正在缓冲");
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            // ⚠ 必须在任何解析之前剥离 ICY 元数据: 否则元数据字节被当音频送
            // 解码器, 产生周期性电流声(用户反馈"音质差"的根因)。
            std::size_t audio_len = icy.process(input, static_cast<std::size_t>(received));
            if (audio_len == 0) continue;   // 本块全是元数据

            // 首块 MP3 同步字检查(在剥离元数据之后做, 否则首字节可能落在元数据里)。
            //
            // ⚠ 不能用"首字节必须是 0xFF/I"来判定 —— 那会**误杀合法的直播流**。
            // 实测重庆都市广播 / 北碚嘉陵之声均为标准 MP3(ffmpeg 解码 0 错误),
            // 但它们是从**帧中间接入**的: 首字节分别是 0x0E / 0x6C, 真正的
            // 帧同步字 0xFF 出现在偏移 101 / 11 处。这类"起始残片"所有解码器
            // 都能容忍, 硬判非 MP3 会让这些台永远播不了。
            // 正确做法: 在首个数据块内**搜索**帧同步字(0xFF 且高 3 位为 1),
            // 找到就把前面的残片丢掉; 找不到(如 HTML 错误页)才判为非法。
            if (!stream_checked) {
                stream_checked = true;
                std::size_t sync = audio_len;   // audio_len = 找不到
                for (std::size_t i = 0; i + 2 < audio_len; ++i) {
                    if (radio_mp3_header_valid(input[i], input[i + 1], input[i + 2])) {
                        sync = i;
                        break;
                    }
                }
                if (sync >= audio_len) {
                    // 前 512 字节内没有任何帧同步字: 极可能不是音频流(错误页等)
                    ESP_LOGW(kTag, "No MP3 sync word in first %u bytes (first=0x%02X url=%.70s), skipping",
                             (unsigned)audio_len, input[0], preset.url);
                    break;
                }
                if (sync > 0) {
                    // 丢掉起始残片, 从同步字开始送解码器
                    ESP_LOGI(kTag, "Stream starts mid-frame; skipping %u lead bytes",
                             (unsigned)sync);
                    std::memmove(input, input + sync, audio_len - sync);
                    audio_len -= sync;
                }
            }
            if (decode_fail_streak >= 8) break;   // 多次短暂停重试仍失败: 交外层冷却

            // ⚠⚠ 前置判据(2026-10-09/10 发热事故的根本修复):
            // MP3 解码器句柄打开 != 首帧缓冲已分配。真正的 ~20KB 连续内存在
            // **首次 esp_audio_simple_dec_process() 内部**才懒分配。
            // 因此本流还没解出首帧(format_ready==false)而 largest 又不足时,
            // 这次调用注定失败, 且底层库单次调用内部会连喷约 20 条
            // "Fail to init MP3 decoder ret 10"(实测每 10~15 秒一轮, 发热)。
            //
            // ✅ 例外(2026-10-10): 若池**已在开机预热成功**(s_pool_warmed,
            // 日志 `decoded=4608`), 说明解码器内部那 ~20KB 早在开机
            // (largest=114688)时就分配好并常驻(我们全程不 reset、不释放池),
            // 播放时**不需要任何新的大块内存** -> 此时必须跳过本判据,
            // 否则会误杀正常播放(实测: 池已预热却被判据拦 -> 收音机永远播不了)。
            // 安全性: 万一 warmed 判断有误、真的解不出来, 下方失败路径的
            // `memory_hopeful=false` 会立即把 decode_fail_streak 置 100 并跳出,
            // **只产生 1 条日志**, 不会形成风暴。
            if (!format_ready && !s_pool_warmed.load(std::memory_order_acquire)) {
                const std::size_t largest_now =
                    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                if (largest_now < kDecoderMinBlock) {
                    static int64_t s_last_skip_warn_us = 0;
                    const int64_t now_us = esp_timer_get_time();
                    if (now_us - s_last_skip_warn_us > 5LL * 1000 * 1000) {
                        s_last_skip_warn_us = now_us;
                        ESP_LOGW(kTag,
                                 "insufficient contiguous heap for decoder first frame "
                                 "(largest=%uB < %uB): skip attempt, back off",
                                 (unsigned)largest_now,
                                 (unsigned)kDecoderMinBlock);
                    }
                    // 标记"这次失败是内存不足引起的" -> 播放循环会走长退避,
                    // 且**不**换台(换到别的台同样解不出来)。
                    s_last_fail_lowmem.store(true, std::memory_order_release);
                    break;   // 不调用解码器 -> 无错误日志 -> 不发热
                }
            }
            esp_audio_simple_dec_raw_t raw = {
                .buffer = input,
                .len = static_cast<uint32_t>(audio_len),
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            while (raw.len > 0 && request_still_current(generation, station)) {
                esp_audio_simple_dec_out_t frame = {
                    .buffer = output,
                    .len = kPcmCapacity,
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                raw.consumed = 0;
                const esp_audio_err_t result =
                    esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    ESP_LOGE(kTag, "Decoder needs %u bytes, buffer has %u",
                             static_cast<unsigned>(frame.needed_size),
                             static_cast<unsigned>(kPcmCapacity));
                    raw.len = 0;
                    break;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    // 解码器对"首帧"会做懒分配(MP3 解析上下文/帧缓冲, 需约 20KB
                    // 连续块)。堆碎片化时返回 ret 10(内存不足) / -2。
                    //
                    // ⚠ 关键判据: **重试前先看堆里有没有足够连续块**。
                    // 若 largest 明显不够, 重试注定失败 —— 只会以 ~5 次/秒的频率
                    // 刷 "Fail to init" 并烧 CPU(实测: 一次 60s 打 107 条错误,
                    // 触发发热告警)。此时必须立刻放弃本块, 交给外层指数退避
                    // (1.5s -> 10s -> ... -> 120s), 而不是原地空转。
                    // 只有当内存看起来够(碎片短时抖动)才值得短暂停顿后重试。
                    const size_t largest =
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                    const bool memory_hopeful = largest >= kDecoderMinBlock;
                    if (++decode_fail_streak == 1)
                        ESP_LOGW(kTag, "MP3 decode failed: %d (largest=%u)",
                                 result, (unsigned)largest);
                    if (memory_hopeful && decode_fail_streak <= 3) {
                        // 让出 CPU 并给堆一点时间合并出连续块, 再试同一个块。
                        vTaskDelay(pdMS_TO_TICKS(200));
                    } else {
                        // 内存不够(或已重试多次): 立即放弃, 不再空转刷屏。
                        // 这里的"失败"由 decode_fail_streak 决定退出读取循环,
                        // 之后走外层退避, 不会形成风暴。
                        if (!memory_hopeful && decode_fail_streak == 1)
                            ESP_LOGW(kTag,
                                     "decoder needs ~%uB contiguous, largest=%uB"
                                     " -> skip retries, back off",
                                     (unsigned)kDecoderMinBlock, (unsigned)largest);
                        decode_fail_streak = 100;   // 直接判定本流不可解, 交给外层
                    }
                    raw.len = 0;
                    break;
                }
                if (raw.consumed > raw.len) raw.consumed = raw.len;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;

                if (frame.decoded_size > 0) {
                    if (join_warmup_frames > 0) {
                        --join_warmup_frames;
                        raw.len = 0;
                        break;   // 丢弃接入期的对齐垃圾帧, 不推给 I2S
                    }
                    s_pool_warmed.store(true, std::memory_order_release);
                    esp_audio_simple_dec_info_t info = {};
                    if (!format_ready &&
                        esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                        source_channels = std::clamp<uint8_t>(info.channel, 1, 2);
                        if (info.bits_per_sample != 16 || info.sample_rate < 8000 ||
                            info.sample_rate > 48000 ||
                            bsp_audio_set_format(info.sample_rate, 16, 1) != ESP_OK) {
                            ESP_LOGE(kTag, "Unsupported stream format: %luHz/%ubit/%uch",
                                     static_cast<unsigned long>(info.sample_rate),
                                     info.bits_per_sample, source_channels);
                            raw.len = 0;
                            break;
                        }
                        applied_volume = s_volume.load(std::memory_order_acquire);
                        bsp_audio_set_volume(applied_volume);
                        if (!s_audio_yield.load(std::memory_order_acquire)) {
                            bsp_audio_set_muted(false);
                        }
                        sample_rate = info.sample_rate;
                        format_ready = true;
                        ESP_LOGI(kTag, "Playing %s at %luHz/%uch -> mono", preset.name,
                                 static_cast<unsigned long>(info.sample_rate), source_channels);
                    }
                    if (format_ready) {
                        // ui_sfx may have opened the codec at 16 kHz while the
                        // radio was yielding. Restore the stream format before
                        // the next write and only then unmute the DAC.
                        if (s_audio_needs_reformat.exchange(false, std::memory_order_acq_rel)) {
                            if (bsp_audio_set_format(sample_rate, 16, 1) != ESP_OK) {
                                s_audio_needs_reformat.store(true, std::memory_order_release);
                                vTaskDelay(pdMS_TO_TICKS(10));
                                continue;
                            }
                            applied_volume = s_volume.load(std::memory_order_acquire);
                            bsp_audio_set_volume(applied_volume);
                            bsp_audio_set_muted(false);
                            ESP_LOGI(kTag, "audio format restored after UI effect: %luHz",
                                     static_cast<unsigned long>(sample_rate));
                        }
                        const uint8_t requested_volume = s_volume.load(std::memory_order_acquire);
                        if (requested_volume != applied_volume) {
                            bsp_audio_set_volume(requested_volume);
                            applied_volume = requested_volume;
                            ESP_LOGI(kTag, "volume applied from player: %u", applied_volume);
                        }
                        const std::size_t mono_bytes =
                            downmix_to_mono(frame.buffer, frame.decoded_size, source_channels);
                        const int64_t write_start = esp_timer_get_time();
                        if (last_write_end) {
                            const uint32_t gap = write_start - last_write_end;
                            max_gap_us = std::max(max_gap_us, gap);
                            if (gap > 30000) ++slow_gaps;
                        }
                        const auto *samples = reinterpret_cast<const int16_t *>(frame.buffer);
                        for (std::size_t i = 0; i < mono_bytes / 2; ++i) {
                            const int32_t value = samples[i];
                            if (value >= 32760 || value <= -32760) ++pcm_clipped;
                            const uint32_t magnitude =
                                static_cast<uint32_t>(value < 0 ? -value : value);
                            pcm_abs_sum += magnitude;
                            if (magnitude > pcm_peak) {
                                pcm_peak = static_cast<uint16_t>(magnitude);
                            }
                        }
                        pcm_samples += mono_bytes / 2;
                        if (s_audio_yield.load(std::memory_order_acquire)) {
                            // 提示音占用音频设备: 这段时间不写 I2S。
                            vTaskDelay(pdMS_TO_TICKS(5));
                            continue;
                        }
                        if (bsp_audio_write(frame.buffer, mono_bytes) != ESP_OK) continue;
                        last_write_end = esp_timer_get_time();
                        if (last_write_end - stats_start >= 10000000) {
                            max_read_us = feed.max_read_us.exchange(0);
                            const uint32_t dbytes = feed.diag_bytes.exchange(0);
                            const uint32_t deagain = feed.diag_eagain.exchange(0);
                            const uint32_t dzero = feed.diag_zero.exchange(0);
                            // 只在吞吐异常时打印: 正常窗口静默(否则这条日志本身
                            // 也会占用串口与栈)。eagain 高说明 socket 长时间收不到
                            // 数据, 是"播几秒卡十几秒"的直接指标。
                            if (deagain >= 3U || dbytes < 20000U) {
                                wifi_ap_record_t apinfo = {};
                                int rssi_dbm = 0;
                                if (esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK) {
                                    rssi_dbm = (int)apinfo.rssi;
                                }
                                ESP_LOGW(kTag,
                                         "low throughput: bytes=%u eagain=%u zero=%u "
                                         "avail=%u rssi=%d",
                                         (unsigned)dbytes, (unsigned)deagain,
                                         (unsigned)dzero, (unsigned)ring.available(),
                                         rssi_dbm);
                            }
                            ESP_LOGI(kTag, "audio stats: wall=%ums pcm=%ums read_max=%ums gap_max=%ums gaps30=%u clipped=%u rms=%u peak=%u rebuffers=%u queued=%u stack_free=%u",
                                     (unsigned)((last_write_end - stats_start) / 1000),
                                     (unsigned)(pcm_samples * 1000ULL / sample_rate),
                                     (unsigned)(max_read_us / 1000), (unsigned)(max_gap_us / 1000),
                                     (unsigned)slow_gaps, (unsigned)pcm_clipped,
                                     pcm_samples ? (unsigned)(pcm_abs_sum / pcm_samples) : 0,
                                     (unsigned)pcm_peak,
                                     (unsigned)rebuffers, (unsigned)ring.available(),
                                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
                            stats_start = last_write_end;
                            max_read_us = max_gap_us = slow_gaps = pcm_samples = pcm_clipped = 0;
                            pcm_abs_sum = 0;
                            pcm_peak = 0;
                            rebuffers = 0;
                        }
                        // The home screen has no spectrum bars; skip the
                        // 16-band RMS pass while only the minimized player is
                        // visible. It runs inside the same critical window as
                        // the I2S write and was part of the 15-18 ms inter-write
                        // gap that could starve the DMA.
                        if (radio_ui_leo_page_visible()) {
                            calculate_levels(frame.buffer, mono_bytes, 1);
                        }
                        if (!playback_announced) {
                            if (!played_audio) {
                            ESP_LOGI(kTag, "PCM playback active (%u bytes)",
                                     static_cast<unsigned>(mono_bytes));
                            }
                            played_audio = true;
                            playback_announced = true;
                            radio_ui_set_playback(RadioPlaybackState::Playing, "正在播放");
                        }
                    }
                }

                if (raw.consumed > 0 || frame.decoded_size > 0) {
                    decode_fail_streak = 0;
                } else {
                    // Keep unconsumed input intact, including after cached PCM output.
                    // Bound a stalled decoder instead of overwriting its pending bytes.
                    if (++decode_fail_streak >= 8) {
                        ESP_LOGW(kTag, "MP3 decoder stalled; reconnecting stream");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            if (decode_fail_streak >= 8) break;
        }
    } while (false);

    if (network_task) {
        // Also cancel on a decoder failure while the user's station is unchanged.
        feed.stop.store(true, std::memory_order_release);
        xTaskNotifyGive(network_task);
        while (!feed.finished.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(5));
        vTaskDelete(network_task);
        // 读线程自己不能打日志(栈太浅), 在这里(4.6KB 栈)统一上报。
        const uint8_t reason = feed.exit_reason.load(std::memory_order_acquire);
        if (reason == 1U) {
            ESP_LOGW(kTag, "network feed idle %us, reconnecting",
                     (unsigned)(kStallBudgetUs / 1000000));
        } else if (reason == 2U) {
            ESP_LOGW(kTag, "network feed stopped by read error");
        }
    }
    // 只释放 heap 兜底路径真正 malloc 出来的块；借用小智麦克风静态栈时
    // heap_network_start == block_count，这里不会误 free 静态内存。
    for (std::size_t i = heap_network_start; i < block_count; ++i) free(network_blocks[i]);
    // 池化资源(开机预分配)不在此释放, 留给下次切台复用。
    if (!s_decoder_pool) {
        if (decoder) esp_audio_simple_dec_close(decoder);
        free(output);
        free(input);
    }
    close_client();
    xSemaphoreGiveRecursive(s_pool_mutex);
    return played_audio;
}

void player_task(void *) {
    std::size_t last_attempted_station = SIZE_MAX;
    uint8_t failed_attempts = 0;
    // 连续失败升级冷却: 坏流(如目录里不可解码的电台)会被反复重试, 固定 10s
    // 冷却等于每 10s 烧一轮 CPU+解码风暴(实测发热)。按连续失败次数指数退避,
    // 上限 120s, 使坏流状态下 CPU 占用可忽略。
    uint32_t fail_backoff_ms = 500;    // 首次短退避: 让堆有时间合并出解码器所需连续块
    // 跳台冷却: 连播失败大多是 CDN 限流/服务端 RST(2026-10-25 实测: 毫秒级
    // 连拨 10 个台全部被 307 拒掉, 间隔仅 20~40ms), 冷却太短等于继续打脸
    // CDN。每次跳台升级 1.5s -> 3s -> 4.5s(封顶), 播出成功即复位。
    uint32_t skip_cooldown_ms = 1500;
    // 连续"涓流会话"计数: 解码出过音频但 prebuffer 到期仍不足目标量(源饿)。
    // 旧判据 played_audio 只要有几十毫秒音频就当活台, 用户被钉死在无声台上。
    uint8_t starved_sessions = 0;
    while (true) {
        if (!s_wanted_playing.load(std::memory_order_acquire)) {
            radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
            xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
            if (!s_wanted_playing.load(std::memory_order_acquire)) {
                s_worker_quiescent = true;
                xSemaphoreGive(s_worker_mutex);
                while (!s_wanted_playing.load(std::memory_order_acquire)) {
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                }
                xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
                s_worker_quiescent = false;
            }
            xSemaphoreGive(s_worker_mutex);
            continue;
        }
        if (!s_network_connected.load(std::memory_order_acquire)) {
            radio_ui_set_playback(RadioPlaybackState::Stopped, "等待网络");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        const std::size_t station = s_station_index.load(std::memory_order_acquire);
        const uint32_t generation = s_generation.load(std::memory_order_acquire);
        if (station != last_attempted_station) {
            // 换台时重置"该台连续失败数", 但【不重置退避】—— 否则两三个坏台
            // 互相切换会让指数退避永远卡在 10s, 每 10s 烧一轮解码风暴(实测发热)。
            last_attempted_station = station;
            failed_attempts = 0;
        }
        s_stream_active.store(true, std::memory_order_release);
        s_last_fail_lowmem.store(false, std::memory_order_release);
        const bool played = stream_station(station, generation);
        s_stream_active.store(false, std::memory_order_release);
        // 请求已失效(换台/暂停/断网): 让出 CPU 再重试。此处若直接 continue,
        // 当 station 请求快速失败时会在无延时循环里空转饿死 IDLE -> task_wdt。
        if (!request_still_current(generation, station)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (played) {
            // The stream worked at least briefly, so this station is alive and
            // simply dropped. Keep it and reconnect.
            failed_attempts = 0;
            skip_cooldown_ms = 1500;
            fail_backoff_ms = 1500;
            // 2026-10-25: "播过 26ms"不等于"台健康"。涓流台(源饿, prebuffer
            // 每轮到期都不足量)会在这里无限重连, 用户被钉死在无声台上。
            // 连续 2 次涓流会话 -> 按"台源有问题"跳台(带冷却)。健康会话复位。
            const bool starved =
                s_last_prebuffer_starved.exchange(false, std::memory_order_acq_rel);
            const std::size_t live_count = radio_player_station_count();
            if (starved) {
                ++starved_sessions;
                // 2026-10-25 实测收口: 涓流会话每轮 20~30s 的卡顿+静音,
                // 用户已经在受苦 —— 1 轮饥饿就直接跳(冷却会让该台稍后
                // 轮回来, 若 CDN 恢复还能听)。阈值 2 的版本让用户每台
                // 白等 40~60s, 不如快跳。
                if (starved_sessions >= 1 && live_count > 1) {
                    ESP_LOGW(kTag, "station %u starved (source feeds too "
                                   "slow); skipping",
                             static_cast<unsigned>(station));
                    radio_ui_set_playback(RadioPlaybackState::Error,
                                          "信号太弱 已换台");
                    starved_sessions = 0;
                    radio_player_select_relative(1);
                    interruptible_delay_ms(skip_cooldown_ms, generation, station);
                    if (skip_cooldown_ms < 4500) skip_cooldown_ms += 1500;
                    continue;
                }
            } else {
                starved_sessions = 0;
            }
            radio_ui_set_playback(RadioPlaybackState::Reconnecting,
                                  "信号中断，正在重连");
            interruptible_delay_ms(1200, generation, station);
            continue;
        }

        // ⚠⚠ 区分两类失败(2026-10-10 发热事故):
        //   A) 电台本身坏(地址失效/编码不支持) -> 换台是对的;
        //   B) **内存连续块不足, 解码器首帧分配不出来** -> 换到任何台都一样解不出,
        //      换台反而会在"每个台都失败"之间循环打转, 无限消耗。
        // 对 B 必须: 不换台 + 用**长退避**(默认 60s 起), 并明确提示用户在等内存。
        if (s_last_fail_lowmem.exchange(false, std::memory_order_acq_rel)) {
            // The minimized radio keeps its playback intent while a canceled
            // XiaoZhi task / Wi-Fi teardown is still holding contiguous RAM.
            // Poll quickly for that block instead of entering the old 60s
            // penalty, which made a healthy station look permanently dead.
            if (fail_backoff_ms < 500) fail_backoff_ms = 500;
            radio_ui_set_playback(RadioPlaybackState::Error, "内存不足 稍后重试");
            ESP_LOGW(kTag,
                     "stream failed: not enough contiguous heap; "
                     "hold same station, retry in %ums (no station switch)",
                     (unsigned)fail_backoff_ms);
            if (!interruptible_delay_ms(fail_backoff_ms, generation, station)) continue;
            if (fail_backoff_ms < 10000) fail_backoff_ms = 10000;
            continue;
        }

        // 未播出: 连接/首包失败按"该目录项已失效"处理, 见上面 kMaxFailedAttempts
        // 的说明; 内存不足的场景已在更早的分支单独处理, 不会走到这里。
        ++failed_attempts;
        const std::size_t count = radio_player_station_count();
        if (failed_attempts >= kMaxFailedAttempts && count > 1) {
            ESP_LOGW(kTag, "Station %u unreachable, skipping to the next one "
                           "(cooldown %ums)",
                     static_cast<unsigned>(station),
                     (unsigned)skip_cooldown_ms);
            radio_ui_set_playback(RadioPlaybackState::Error,
                                  "电台无法播放 已换台");
            failed_attempts = 0;
            radio_player_select_relative(1);
            // ⚠ 跳台必须冷却(2026-10-25): 失败后立即连拨会被 CDN 判定为
            // 攻击 -> 整表 307(实测), 反而把活台也拒了。冷却期间用户按键
            // (generation 变化)会立即打断, 不影响手动切台手感。
            interruptible_delay_ms(skip_cooldown_ms, generation, station);
            if (skip_cooldown_ms < 4500) skip_cooldown_ms += 1500;
            continue;
        }

        radio_ui_set_playback(RadioPlaybackState::Error, "连接失败，正在重试");
        if (!interruptible_delay_ms(fail_backoff_ms, generation, station)) continue;
        if (fail_backoff_ms < 3000) {
            fail_backoff_ms = fail_backoff_ms < 1000 ? 1000 : fail_backoff_ms * 2;
        }
    }
}

}  // namespace

static bool start_player_task_locked() {
    if (s_player_task.load(std::memory_order_acquire)) return true;
    TaskHandle_t task = nullptr;
    s_worker_quiescent = false;
    task = xTaskCreateStatic(player_task, "radio_stream", 4608, nullptr, 5,
                             s_player_task_stack, &s_player_task_tcb);
    if (task == nullptr) {
        return false;
    }
    s_player_task.store(task, std::memory_order_release);
    return true;
}

static bool set_playing_locked(bool playing) {
    // 播放意图没变且 worker 还活着时, 直接返回 —— 不能在这里推进 generation。
    // 否则"已最小化播着收音机 -> 进完整收音机界面"的 set_playing(true)
    // 会把正在播的流整条掐断重连, 实测表现为进收音机要等 8 秒以上才出声。
    if (s_wanted_playing.load(std::memory_order_acquire) == playing) {
        if (!playing || s_player_task.load(std::memory_order_acquire) != nullptr) {
            return true;
        }
    }
    s_playback_revision.fetch_add(1);
    s_wanted_playing.store(playing, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    const bool started = !playing || start_player_task_locked();
    if (!started) s_wanted_playing.store(false, std::memory_order_release);
    if (playing && started) xTaskNotifyGive(s_player_task.load(std::memory_order_acquire));
    return started;
}

RadioCatalogPause radio_player_pause_for_catalog(void) {
    xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
    const bool was_playing = s_wanted_playing.load();
    set_playing_locked(false);
    RadioCatalogPause pause{was_playing, s_playback_revision.load()};
    xSemaphoreGive(s_worker_mutex);
    return pause;
}

// 目录搜索把堆切碎后, 播放任务可能第一次创建失败。堆会在目录任务退出后
// 逐渐合并, 所以用这个自带退避的 worker 反复尝试, 直到播放任务真的起来。
// 没有它, 一次失败就意味着"界面永远停在正在切换电台"(实测)。
static void playback_retry_task(void *)
{
    for (int attempt = 0; attempt < 20 && !s_player_task.load(std::memory_order_acquire); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_wanted_playing.load(std::memory_order_acquire)) break;
        if (s_player_task.load(std::memory_order_acquire)) break;
        if (xSemaphoreTake(s_worker_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        const bool ok = start_player_task_locked();
        if (ok) {
            const TaskHandle_t task = s_player_task.load(std::memory_order_acquire);
            if (task) xTaskNotifyGive(task);
        }
        xSemaphoreGive(s_worker_mutex);
        if (ok) {
            ESP_LOGI(kTag, "playback recovered after catalog (attempt %d)", attempt + 1);
            break;
        }
    }
    vTaskDelete(nullptr);
}

static void radio_schedule_playback_retry()
{
    static volatile bool s_retry_queued = false;
    bool expected = false;
    if (!s_retry_queued) {
        s_retry_queued = true;
        if (xTaskCreate(playback_retry_task, "radio_retry", 3072, nullptr, 4, nullptr) != pdPASS) {
            s_retry_queued = false;
            return;
        }
        ESP_LOGW(kTag, "queued playback retry worker");
    }
    (void)expected;
}
void radio_player_resume_after_catalog(RadioCatalogPause pause) {
    xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
    // ⚠ 目录搜索结束后必须让播放**一定**能回来(2026-10-12 修复"收音机一直卡在
    //   正在切换电台")。
    //
    //   原实现在 start_player_task_locked() 失败时直接 s_wanted_playing=false,
    //   于是用户看到的就是: 界面停在"正在切换电台"像卡死, 再按 OK 也只是
    //   再调一次 set_playing(true), 而播放任务依然起不来。日志对应:
    //     radio_player: playback task could not restart after catalog
    //
    //   根因: 搜索期间堆被 8KB 响应缓冲 + 6144 栈的 catalog 任务切碎, 剩下的
    //   ~4.6KB 连续块不足以创建 4608 栈的 radio_stream 任务。
    //
    //   现在: 即使这次没起来, 也保留播放意图, 并交给一个自带退避的 worker
    //   继续重试 —— 堆稍并在目录任务退出后自然合并, 播放会自己爬回来。
    bool restarted = false;
    if (pause.was_playing) {
        s_wanted_playing.store(true, std::memory_order_release);
        restarted = start_player_task_locked();
        if (restarted) {
            const TaskHandle_t task = s_player_task.load(std::memory_order_acquire);
            if (task) xTaskNotifyGive(task);
        }
    }
    xSemaphoreGive(s_worker_mutex);
    if (pause.was_playing && !restarted) {
        ESP_LOGE(kTag, "playback restart failed after catalog; scheduling retry worker");
        radio_schedule_playback_retry();
    }
    radio_wifi_set_streaming(pause.was_playing && s_wanted_playing.load());
}
bool radio_player_init() {
    s_station_mutex = xSemaphoreCreateMutex();
    s_worker_mutex = xSemaphoreCreateMutex();
    if (!s_pool_mutex) s_pool_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_station_mutex || !s_worker_mutex || !s_pool_mutex) return false;
    std::memcpy(s_stations, kFallbackStations, sizeof(kFallbackStations));
    s_station_count.store(sizeof(kFallbackStations) / sizeof(kFallbackStations[0]));

    const uint8_t saved_station = load_setting("station", 0);
    const uint8_t saved_volume = load_setting("volume", 55);
    nvs_handle_t catalog_handle;
    if (nvs_open("radio", NVS_READONLY, &catalog_handle) == ESP_OK) {
        std::size_t bytes = sizeof(s_stations);
        // catalog_v3: 2026-10-18 又一次改名。目录请求从 esp_http_client 换成裸 socket 之后
        // 才真正搜得到本地台, 而此前存进 NVS 的是"只剩全国兜底台"的列表; 不清掉的话
        // app_radio_request_catalog 会因为"已有缓存"永远不再搜索, 用户看到的仍然是外地台。
        if (nvs_get_blob(catalog_handle, "catalog_v3", s_stations, &bytes) == ESP_OK &&
            bytes > 0 && bytes % sizeof(RadioStation) == 0 && bytes <= sizeof(s_stations)) {
            const std::size_t cached_count = bytes / sizeof(RadioStation);
            bool valid = true;
            for (std::size_t i = 0; i < cached_count; ++i) {
                valid = valid && std::memchr(s_stations[i].name, 0, sizeof(s_stations[i].name)) &&
                        std::memchr(s_stations[i].url, 0, sizeof(s_stations[i].url)) &&
                        std::strncmp(s_stations[i].url, "http", 4) == 0;
            }
            if (valid) {
                s_station_count.store(cached_count);
                s_catalog_cached = true;
                ESP_LOGI(kTag, "Restored cached radio catalog: %u stations", (unsigned)cached_count);
            } else {
                std::memcpy(s_stations, kFallbackStations, sizeof(kFallbackStations));
            }
        }
        nvs_close(catalog_handle);
    }
    s_station_index.store(saved_station % radio_player_station_count());
    s_volume.store(std::clamp<uint8_t>(saved_volume, 10, 100));

    if (bsp_audio_init() != ESP_OK) return false;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    bsp_audio_set_volume(s_volume.load());

    radio_ui_set_station(s_station_index.load(), radio_player_station_count(),
                         radio_player_station(s_station_index.load()));
    radio_ui_set_volume(s_volume.load());
    // 优先级 5: 必须**高于** LVGL 刷新任务(默认 prio=4)。此前降到 3 低于 UI,
    // 导致 UI 刷新(尤其电平条)抢占播放任务, I2S DMA 断流 ->
    // 实测"声音一顿一顿 + 电流声"。5 既能压过 UI 保证不断流, 又因 I2S DMA
    // 缓冲已加大, 写入会在缓冲满时正常阻塞让出 CPU, 不会饿死 IDLE。
    // 优先级 5 (> LVGL 默认 4) 是 2026-10-08 验证过的值, 保持不动。
    xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
    bool started = start_player_task_locked();
    xSemaphoreGive(s_worker_mutex);
    return started;
}

void radio_player_set_network(bool connected) {
    s_network_connected.store(connected, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    // 播放意图可能早于联网就绪(开机首页自动播放、WiFi 重连): 那时
    // radio_wifi_set_streaming(true) 因 IsConnected()==false 被丢弃, 之后
    // 再没人把射频切回 PERFORMANCE, 整条流就跑在 modem-sleep 下 -> 周期性
    // 卡顿(实测日志: 全程只有 BALANCED (MIN_MODEM), 没有 PERFORMANCE)。
    // 联网状态变化时按当前播放意图重新应用一次。
    radio_wifi_set_streaming(connected && s_wanted_playing.load(std::memory_order_acquire));
}

void radio_player_toggle() {
    radio_player_set_playing(!radio_player_is_playing());
}

void radio_player_set_playing(bool playing) {
    if (!s_worker_mutex) return;
    xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
    const bool started = set_playing_locked(playing);
    xSemaphoreGive(s_worker_mutex);
    if (!started) {
        ESP_LOGE(kTag, "playback task could not restart");
        radio_ui_set_playback(RadioPlaybackState::Error, "播放内存不足");
    }
    if (!playing) radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
    radio_wifi_set_streaming(playing && started);
}

bool radio_player_is_playing() {
    return s_wanted_playing.load(std::memory_order_acquire);
}

bool radio_player_is_stream_active() {
    return s_stream_active.load(std::memory_order_acquire);
}

bool radio_player_wait_idle(uint32_t timeout_ms) {
    if (!s_worker_mutex) return !s_stream_active.load(std::memory_order_acquire);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true) {
        xSemaphoreTake(s_worker_mutex, portMAX_DELAY);
        TaskHandle_t task = s_player_task.load(std::memory_order_acquire);
        const bool stopping = !s_wanted_playing.load(std::memory_order_acquire);
        const bool stream_idle = !s_stream_active.load(std::memory_order_acquire);
        if (task && stopping && stream_idle) {
            /* Once stream_station has returned, the worker owns no HTTP client,
             * decoder, ring buffer, or pool lock. It may still be waiting in a
             * retry delay, so do not require its quiescent flag: remove the
             * leftover worker now and return its task stack to XiaoZhi. */
            vTaskSuspend(task);
            vTaskDelete(task);
            s_player_task.store(nullptr, std::memory_order_release);
            s_worker_quiescent = false;
            task = nullptr;
        }
        bool idle = stream_idle && (!stopping || !task);
        xSemaphoreGive(s_worker_mutex);
        if (idle) return true;
        if (static_cast<int32_t>(deadline - xTaskGetTickCount()) <= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void radio_player_select_relative(int delta) {
    const int count = static_cast<int>(radio_player_station_count());
    const int current = static_cast<int>(s_station_index.load());
    const std::size_t next = static_cast<std::size_t>((current + delta + count) % count);
    radio_player_set_station(next);
}

void radio_player_set_station(std::size_t index) {
    index %= radio_player_station_count();
    s_station_index.store(index, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    const RadioStation chosen = radio_player_station(index);
    // 切台时把"索引 + 名字"合并成一次 NVS 事务。
    // 原来是 save_setting() 与 save_station_name() 各开一次 NVS 句柄并 commit,
    // 每次切台等于两轮 flash 擦写, 会阻塞几十~上百毫秒并驱逐 NVS 页缓存 ——
    // 用户感知就是"切台要卡很久"。
    {
        nvs_handle_t handle;
        if (nvs_open("radio", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u8(handle, "station", static_cast<uint8_t>(index));
            if (chosen.name[0]) nvs_set_str(handle, "station_name", chosen.name);
            else nvs_erase_key(handle, "station_name");
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
    radio_ui_set_station(index, radio_player_station_count(), radio_player_station(index));
    if (radio_player_is_playing()) {
        radio_ui_set_playback(RadioPlaybackState::Connecting, "正在切换电台");
    }
}

std::size_t radio_player_station_index() {
    return s_station_index.load(std::memory_order_acquire);
}

bool radio_player_has_cached_catalog() { return s_catalog_cached; }

// Drop the in-RAM cache so the next app_radio_request_catalog() performs a real
// search. Used when the remembered city no longer matches the cached catalog's
// city (a failed NVS write can leave the previous city's blob in place).
void radio_player_invalidate_catalog_cache() {
    s_catalog_cached = false;
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "catalog_v3");
        nvs_commit(handle);
        nvs_close(handle);
    }
}

std::size_t radio_player_station_count() {
    return std::max<std::size_t>(s_station_count.load(std::memory_order_acquire), 1);
}

RadioStation radio_player_station(std::size_t index) {
    return station_snapshot(index);
}

// 选城市时从预设表加载该城市的实测可持续台(2026-10-26 普查)。
// 组键匹配: 城市名 -> 省/市组(kCityPresetGroups), 无匹配返回 false(调用方
// 回退到在线目录搜索)。拷贝受 kMaxStations 上限保护。
bool radio_player_load_city_preset(const char *city) {
    if (city == nullptr || city[0] == 0) return false;
    const std::size_t group_count =
        sizeof(kCityPresetGroups) / sizeof(kCityPresetGroups[0]);
    for (std::size_t g = 0; g < group_count; ++g) {
        if (std::strcmp(kCityPresetGroups[g].group, city) != 0) continue;
        const std::size_t offset = kCityPresetGroups[g].offset;
        const std::size_t count = kCityPresetGroups[g].count;
        if (offset + count >
            sizeof(kCityPresetStations) / sizeof(kCityPresetStations[0]))
            return false;
        if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
            return false;
        std::memcpy(s_stations, &kCityPresetStations[offset],
                    count * sizeof(RadioStation));
        s_station_count.store(count, std::memory_order_release);
        s_station_index.store(0, std::memory_order_release);
        s_catalog_cached = true;
        xSemaphoreGive(s_station_mutex);
        ESP_LOGI(kTag, "city preset loaded: %s -> %u stations",
                 city, (unsigned)count);
        return true;
    }
    return false;
}

bool radio_player_replace_stations(const RadioStation *stations, std::size_t count) {
    std::size_t selected = 0;
    if (!install_stations(stations, count, nullptr, &selected)) return false;

    save_setting("station", static_cast<uint8_t>(selected));
    const RadioStation chosen = radio_player_station(selected);
    save_station_name(chosen.name);
    radio_ui_set_station(selected, radio_player_station_count(), chosen);
    ESP_LOGI(kTag, "Installed local catalog with %u stations",
             static_cast<unsigned>(radio_player_station_count()));
    return true;
}

bool radio_player_replace_stations_resuming(const RadioStation *stations,
                                           std::size_t count) {
    char remembered[sizeof(RadioStation::name)] = {};
    const bool has_remembered = load_station_name(remembered, sizeof(remembered));

    // 【顺序策略】本地城市台排**前**, 实测可播的兜底台排**后**。
    //
    // 2026-10-09 修正: 早期把兜底台放前面, 理由是"公共目录里 CN 电台多为
    // 加密/防盗链流"。但实测发现那个结论**只对 https 版本成立** ——
    // radio-browser 返回的 `https://lhttp.qtfm.cn/...` 经 normalize_stream_url
    // 降级为 `http://` 后**完全可播**(实测重庆/北京/上海/广州多个城市台
    // 剥离 ICY 后 MP3 帧都正常, metaint=16000)。且用户明确要"本地电台"。
    // 故改为: 目录搜索结果(本地城市台)优先, 兜底台随后补齐。
    //
    // 曾经试过在 catalog 里逐条探测首字节过滤坏源, 但 esp_http_client 每次
    // 分配较大缓冲, 在 6KB 栈的 catalog 任务里连续探测会让堆从 50KB 掉到 6KB
    // 并触发 wifi:mem fail —— 代价太大, 已放弃。改为"播放时首字节校验 + 退避换台"
    // (radio_player.cc 内已有) 兜住。
    // static: RadioStation 约 418B, 14 项 = 5.7KB, 放栈上会撑爆 6KB 的
    // radio_catalog 任务栈(实测 Stack protection fault 循环重启)。
    auto *merged = static_cast<RadioStation *>(calloc(kMaxStations, sizeof(RadioStation)));
    if (!merged) return false;
    std::size_t merged_count = 0;
    // 1) 先放目录搜索结果(本地城市台 / 全国热门), 去重
    for (std::size_t i = 0; i < count && merged_count < kMaxStations; ++i) {
        bool dup = false;
        for (std::size_t j = 0; j < merged_count; ++j) {
            if (std::strcmp(merged[j].url, stations[i].url) == 0) { dup = true; break; }
        }
        if (!dup) merged[merged_count++] = stations[i];
    }
    // 2) 再用实测可播的兜底台补齐(去重), 保证列表里始终有稳定能出声的台
    for (std::size_t i = 0; i < sizeof(kFallbackStations) / sizeof(kFallbackStations[0]) &&
                            merged_count < kMaxStations; ++i) {
        bool dup = false;
        for (std::size_t j = 0; j < merged_count; ++j) {
            if (std::strcmp(merged[j].url, kFallbackStations[i].url) == 0) { dup = true; break; }
        }
        if (!dup) merged[merged_count++] = kFallbackStations[i];
    }

    std::size_t selected = 0;
    if (!install_stations(merged, merged_count, has_remembered ? remembered : nullptr,
                          &selected)) {
        free(merged);
        return false;
    }
    s_catalog_cached = true;
    nvs_handle_t catalog_handle;
    if (nvs_open("radio", NVS_READWRITE, &catalog_handle) == ESP_OK) {
        // Erase first: overwriting a ~6KB blob in the 24KB NVS partition can
        // fail with ESP_ERR_NVS_NOT_ENOUGH_SPACE while the old entry still
        // occupies its slot, which then left the previous city's catalog in
        // place across a reboot. Releasing the old blob makes room.
        const esp_err_t erased = nvs_erase_key(catalog_handle, "catalog_v3");
        if (erased != ESP_OK && erased != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(kTag, "catalog cache erase failed: %s", esp_err_to_name(erased));
        }
        esp_err_t saved = nvs_set_blob(catalog_handle, "catalog_v3", merged,
                                      merged_count * sizeof(RadioStation));
        if (saved == ESP_OK) saved = nvs_commit(catalog_handle);
        if (saved != ESP_OK) ESP_LOGW(kTag, "catalog cache save failed: %s", esp_err_to_name(saved));
        nvs_close(catalog_handle);
    }
    free(merged);

    save_setting("station", static_cast<uint8_t>(selected));
    const RadioStation chosen = radio_player_station(selected);
    save_station_name(chosen.name);
    radio_ui_set_station(selected, radio_player_station_count(), chosen);
    if (has_remembered && selected > 0) {
        ESP_LOGI(kTag, "Resumed remembered station '%s' at index %u",
                 chosen.name, static_cast<unsigned>(selected));
    } else {
        ESP_LOGI(kTag, "Installed local catalog with %u stations",
                 static_cast<unsigned>(radio_player_station_count()));
    }
    return true;
}

void radio_player_set_volume(uint8_t volume) {
    volume = std::clamp<uint8_t>(volume, 10, 100);
    s_volume.store(volume, std::memory_order_release);
    // 播放任务连续占用音频锁时，UI 同步等锁会被饿死；交给下一帧应用。
    save_setting("volume", volume);
    radio_ui_set_volume(volume);
}

uint8_t radio_player_volume() {
    return s_volume.load(std::memory_order_acquire);
}

// ---- C-linkage bridge for xiaozhi_client.c ----
extern "C" void radio_audio_yield(bool yield) {
    if (yield) {
        s_audio_yield.store(true, std::memory_order_release);
        return;
    }
    // Publish the reformat request before releasing the stream loop. The loop
    // checks the pending flag before its next write, so it can never push a
    // 48 kHz frame through the 16 kHz codec configuration left by ui_sfx.
    s_audio_needs_reformat.store(true, std::memory_order_release);
    s_audio_yield.store(false, std::memory_order_release);
}
extern "C" bool radio_audio_yielded(void) {
    return s_audio_yield.load(std::memory_order_acquire);
}

extern "C" void radio_pause_for_xiaozhi(void) {
    radio_player_set_playing(false);
    radio_player_wait_idle(2000);
}
extern "C" bool radio_is_playing_c(void) {
    return radio_player_is_playing();
}
