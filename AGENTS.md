# AGENTS.md

## 项目说明

本项目为 ESP32（ESP-IDF v5.5.3）项目。

ESP-IDF 工具链路径：`C:\esp\v5.5.3\esp-idf`，如遇基础库或组件缺失，优先在该路径中查找。

上位服务端路径: `..\xiaozhi-esp32-server`

---

## 板级说明

当前开发板：`atk-dnesp32s3`

板级相关代码目录：`main/boards/atk-dnesp32s3`

限制：
- 禁止修改 `main/board` 下其他目录，`main/board` 下其他目录不会被编译

---

## 编译环境

如果运行在 WSL：
- 仅进行静态检查

---

## 环境初始化

在 Windows PowerShell 中执行：

```powershell
. C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1
```

---

## 编译命令

在执行了环境初始化的会话中执行：

```powershell
idf.py build
```
