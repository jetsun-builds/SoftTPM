# SoftTPM Windows 11 x64 测试签名包

适用目标：Windows 11 24H2/25H2/26H2 x64；本包为无硬件 TPM 的电脑准备。
不需要安装 Visual Studio、WDK、OpenSSL 或 devcon。请完整解压，保留目录结构。

这是研究项目的测试签名构建，不是微软正式签名驱动。CI 检查编译、签名、
驱动目录文件和 EXE 依赖加载；没有在你的 T7820 上加载内核驱动，也没有验证
Windows Update 会推送 26H2。不要将其用作系统盘 BitLocker 的 TPM 保护器。

## 安装前

1. 备份重要数据；若使用 BitLocker，保存恢复密钥并暂停保护。
2. 在 BIOS 中关闭 Secure Boot。
3. 打开“Windows PowerShell（管理员）”，执行以下命令，再自行重启：

```powershell
bcdedit /set testsigning on
```

此命令仅开启测试签名；不要额外设置 nointegritychecks。安装脚本不会自动改 BCD、
关闭安全功能、停用真实 TPM 或强制重启。

## 安装与验证

在解压后的目录打开 **64 位 Windows PowerShell（管理员）**：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./Install-SoftTPM.ps1
```

脚本会验证 SHA256、导入随包的测试签名公钥证书（本机 Root 与 TrustedPublisher）、
安装 Root\SoftTpm 设备、复制后台服务并设置开机自动启动。证书指纹及构建提交见
build-info.json；工作流摘要也会显示证书指纹。包内不含签名私钥。

如提示需要重启，先重启。然后运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./Test-SoftTPM.ps1
```

预期：Get-Tpm 的 TpmPresent/TpmReady 为 True，tpmtool 查询成功，
SoftTpmStatus 输出 SOFTTPM SMOKE TEST PASSED。再重启一次并重复检查持久化状态。

如果设备出现 Code 52、Code 39 或服务无法启动，先保留原状并查看设备管理器、
系统事件日志以及 `%ProgramData%/SoftTpm/softtpm.log`；不要清除 TPM 状态，
也不要为了安装自动关闭内存完整性等其他功能。编译通过不等于所有系统配置可运行。

## 卸载

使用安装时的同一份完整包，管理员执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./Uninstall-SoftTPM.ps1
```

该脚本只移除 SoftTPM 的服务、匹配的设备和驱动包、该构建的测试证书，保留
`%ProgramData%/SoftTpm` 中的 TPM 身份和密钥。卸载后重启。
如测试签名只为本次试验开启，可随后管理员执行 `bcdedit /set testsigning off`，
再重启，并按需恢复 Secure Boot 与 BitLocker 保护。

## 构建内容

- driver/：签名后的 SYS、INF、CAT。
- service/：后台服务、OpenSSL DLL、应用本地 Visual C++ 运行库。
- tools/：SoftTpmStatus.exe。
- SoftTPM-TestSigning.cer：每次构建新建的测试签名公钥证书，未包含私钥。
- SHA256SUMS.txt、build-info.json：完整性、源码版本与验证范围。
- licenses/：SoftTPM、微软 TPM 参考实现和 OpenSSL 许可。

源码：https://github.com/jetsun-builds/SoftTPM
