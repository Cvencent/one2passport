#include "freq.h"  // extracted from main/radio_catalog.cc by run_tests.sh
#include <cstdio>
#include <cstring>
struct Case { const char *name; int expect; };
int main() {
    Case cases[] = {
        {"上海经典音乐广播 FM94.7", 947},
        {"上海经典金曲广播 FM103.7", 1037},
        {"上海流行音乐广播 动感101 FM101.7", 1017},
        {"上海第一财经广播 FM90.9", 909},
        {"上海新闻广播 FM93.4 AM990", 934},
        {"上海浦东FM106.5东上海之声", 1065},
        {"深圳飞扬971 FM97.1", 971},
        {"深圳交通广播 FM106.2", 1062},
        {"深圳私家车广播 FM94.2", 942},
        {"杭州城市资讯广播 FM90.7杭州潮流音乐电台", 907},
        {"金堂综合广播FM88.9 成都年代音乐怀旧好声音", 889},
        {"沈阳新民广播FM103.9", 1039},
        {"大连金普新区FM104.3 综合广播", 1043},
        {"FM99.1大连都市之声广播", 991},
        {"大连少儿广播 FM106.7", 1067},
        {"MY FM全国音乐频道——FM106.3  长春少儿与老年生活广播", 1063},
        {"长春经济广播U FM88.0", 880},
        {"Shanghai FM 101 上海动感101", 1010},
        {"深圳龙岗星光991", 991},
        {"望城综合广播 长沙925电台", 925},
        {"长沙Big Radio 快乐886", 886},
        {"城阳综合广播 青岛广播爱车940", 940},
        // must NOT match
        {"北京新闻广播", 0},
        {"长沙交通广播", 0},
        {"广州经济交通广播", 0},
        {"成都故事广播", 0},
        {"CRI劲曲调频 HIT FM（成都）", 0},
        {"上海新闻广播", 0},
        {"深圳龙岗频道", 0},
        {"广州市番禺区广播电台", 0},
        {"长沙市望城区人民广播电台", 0},
        {"MY FM全国音乐频道·广州", 0},
        {"杭州华语之声网络广播", 0},
    };
    int fail = 0;
    for (auto &c : cases) {
        int got = freq::parse_frequency(c.name);
        if (got != c.expect) {
            std::printf("FAIL  expect %4d got %4d  %s\n", c.expect, got, c.name);
            ++fail;
        }
    }
    std::printf(fail ? "\n%d case(s) failed\n" : "\nall %d cases passed\n",
                fail ? fail : (int)(sizeof(cases)/sizeof(cases[0])));
    return fail ? 1 : 0;
}
