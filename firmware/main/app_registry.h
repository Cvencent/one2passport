#pragma once
// app_registry.h —— V6 通用应用框架
//
// 参照原厂 ai-passport 的 demo.h「entry 结构 + 注册表」模式:
//   新增应用 = 实现一份 enter/exit/key + 在 APPS[] 里加一项, 菜单自动多一格。
// 与原厂差异(按 V6 设计稿):
//   - 菜单是手表式单图标轮播, 不是宫格; 轮播渲染读本表
//   - 全局返回: 长按UP 在应用内 = 回菜单; 在菜单 = 回首页; 首页长按UP = 进菜单
//   - 生命周期: enter 持 LVGL 键建页; exit 持锁删页; key 由 input_task 调
//   - audio_start/audio_stop 可选: 进应用前停收音机(ES8311 只有一路)

#include "bsp_button.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_ID_RADIO = 0,
    APP_ID_CODEX,
    APP_ID_XIAOZHI,
    APP_ID_SETTINGS,
    APP_ID_COUNT
} app_id_t;

typedef struct {
    const char *name;        // 菜单里显示的应用名(中文)
    // 进入应用: 持 LVGL 锁, 创建并载入本应用页面。返回 false = 创建失败
    bool (*enter)(void);
    // 退出应用: 持 LVGL 锁, 删除页面、恢复首页前的清理
    void (*exit)(void);
    // 应用内按键: 由 input_task 调; 实现自行持 LVGL 锁
    // 返回 true = 长按UP 已被本应用处理(一般返回 false, 让框架统一回菜单)
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);
    // 可选: 进入应用前停收音机(不持锁)。NULL = 不需要
    void (*audio_pause)(void);
    // 可选: 退出应用后恢复音频。NULL = 不需要
    void (*audio_resume)(void);
} app_entry_t;

// 注册表(定义在 app_registry.cc): 菜单轮播顺序 = 数组顺序
extern const app_entry_t APPS[APP_ID_COUNT];

// ---- 框架运行时 ----
// 初始化框架: 建首页/菜单, 装输入分发。app_main 里显示初始化后调用
void app_framework_init(void);

// input_task 调: 按键事件进框架统一分发(全局导航+应用转发)
void app_framework_dispatch(bsp_btn_t btn, bsp_btn_ev_t ev);

// 系统级回首页(配网退出/异常恢复): 退出当前应用并载入首页桌面
void app_framework_go_home(void);
void app_framework_go_menu(void);

// 当前所在层级(供外部如 Codex 桥接查询/驱动)
typedef enum {
    APP_LEVEL_HOME = 0,   // 首页桌面
    APP_LEVEL_MENU,       // 应用菜单
    APP_LEVEL_APP,        // 某个应用内
} app_level_t;

app_level_t app_framework_level(void);
app_id_t    app_framework_active(void);   // APP_LEVEL_APP 时有效

// Codex 状态驱动首页宠物(由桥接任务线程调用; 内部自己拿 LVGL 锁)
typedef enum {
    CODEX_STATE_IDLE = 0,
    CODEX_STATE_BUSY,
    CODEX_STATE_DONE,
    CODEX_STATE_FAIL,
    CODEX_STATE_OFFLINE,
    CODEX_STATE_ATTENTION,
} codex_state_t;
void app_home_set_codex_state(codex_state_t state);

#ifdef __cplusplus
}
#endif
