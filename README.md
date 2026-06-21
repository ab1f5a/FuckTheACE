# Fuck The Ace

降低 ACE 反作弊扫盘带来的游戏卡顿。

## 原理

监听 `SGuardSvc64.exe` / `SGuard64.exe` / `ACE-Tray.exe`，一旦发现：
1. 设置进程优先级为低（`IDLE_PRIORITY_CLASS`）
2. 绑定 CPU 亲和性到最后一个逻辑核

每轮询周期检查一次是否被 ACE 还原，被还原则重新压制。

## 编译

需要 MinGW-w64，推荐用 winget 安装：

```
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

然后运行 `build.bat`。产物为 `fucktheace.exe`，约 62KB，静态链接无依赖。

## 使用

右键以管理员身份运行 `fucktheace.exe`，程序驻留系统托盘。

托盘右键菜单：
- **查看日志** — 打开 `%APPDATA%\fucktheace\fucktheace.log`
- **开机时启动** — 通过任务计划程序实现，不弹 UAC
- **监控间隔** — 实时 / 10s / 30s / 60s / 120s / 300s
- **退出程序**

配置保存在 `C:\fucktheace\config.ini`，重启后恢复上次设置。

## 系统要求

- Windows 10 / 11 64 位
- 管理员权限（设置进程优先级和 CPU 亲和性需要）
