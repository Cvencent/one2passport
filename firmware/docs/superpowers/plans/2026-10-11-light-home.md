# 首页轻量运行 Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 首页保留收音机音频与 Codex 状态核心，完整操作资源仅在应用内驻留。
**Architecture:** 保持现有协议与单播放器；将 Codex 渲染/目录缓存移入按需工作区，退出收音机销毁页面，目录发现只在完整应用触发。
**Tech Stack:** ESP-IDF / C / C++ / LVGL / Python serial verification.
**Spec:** ../specs/2026-10-11-light-home-design.md

## Global Constraints
首页通知与连接不中断；保留解码器早期预热；目录失败保留缓存。

## Review Focus
重复进入/退出；申请失败；退出后收到任务完成；Wi-Fi 重连；后台音频仍可播放。

### Task 1: Codex 工作区生命周期
- [ ] 创建主机侧测试，检查初始未申请、重复申请幂等、退出释放、失败无半成品。
- [ ] 实现 coding_workspace.h 的 acquire/release，渲染文本与项目目录使用工作区；完整页退出释放；首页连接不请求项目目录。
- [ ] 主机测试通过与固件编译成功。

### Task 2: 收音机轻量首页
- [ ] 实机现状日志作为失败基线：目录开机自动发起、首页到播放路径最大连续堆仅 1.5KB。
- [ ] radio_ui_leave_leo_page 清理完整页面对象并停刷新，保留数据；退出应用保留播放。Wi-Fi 连接只更新播放网络状态，完整应用进入才刷新目录。
- [ ] 刷机核对首页与进入应用内存、目录、PCM、返回首页与重入。

### Task 3: 验证与记录
- [ ] 运行现有收音机核心测试，构建/刷机 hash 校验，串口走完整生命周期。
- [ ] 记录实际通过项与未通过项，不把单台成功推广为全部电台。
