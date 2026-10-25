<div align="center">

<img src="docs/brand/logo.svg" width="112" alt="Prism NR 标志：一束白光射入细线棱镜，分成四道光谱" />

# Prism NR

**一束光进去，一道光谱出来。**
自带运行时的 DLSS 5 神经渲染管理器。一键启用，有守护，可还原。

![Version](https://img.shields.io/badge/version-1.5.1-000000)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-000000)
![GPU](https://img.shields.io/badge/GPU-RTX%2050-000000)
![License](https://img.shields.io/badge/license-MIT-000000)

[English](README.md) ·
[官网](https://prism.chefzc.dev) ·
[Prism Core](native/prism-core/README.md) ·
[更新日志](CHANGELOG.md) ·
[安全模型](SECURITY.md) ·
[设计系统](DESIGN.md)

</div>

![Prism NR 海报：黑底上的发丝条码场、正弦格栅和 Prism NR 字标](docs/brand/poster.svg)

Prism NR 找到你的游戏，读每个游戏的主程序，把自研的 DLSS 5 神经渲染运行时 **Prism Core** 装进去：不需要 ReShade、OptiScaler、RenoDX，也不用下载任何东西。Core 自己运行 NVIDIA 签名版网络，不让人脸、暗部和高光被"重新生成"，所有设置都能在游戏内面板里实时调。每次启用都是一次完整的事务，还原能把游戏目录原样放回。

![地平线 6 中打开 Prism Core 游戏内面板：帧时间条码、档位、色彩风格、强度、遍数和节奏](docs/screenshots/overlay-fh6.jpg)

## 1.5 新增

- **账号（可选）**：邮箱验证码注册；邮箱、手机号、用户名都能登录。个人名片有头像、简介、创始人印章、贡献者标记和铭牌。不登录也完全照常使用。
- **社区按游戏分类**：206 款游戏共 623 套官方调色，新作和经典老游戏都有，每款游戏一套专属调色，再配两套适合它类型的风格。中文、英文、简称都能搜（地平线6、fh6、大表哥、cp2077），你库里有的游戏排最前。
- **正在玩的游戏排最前**，一键导入，运行中的游戏约 1 秒生效，不用重启。
- **调色码**：调色台导出一行文字（`PRISM1.…`），带着它是给哪个游戏调的，还有校验。
- **双显卡**：核显 + 独显的笔记本、双卡台式机，游戏自动设为 Windows 的"高性能"显卡，网络固定跑在 NVIDIA 卡上（可以在设置里选）。*副卡运行*（Offload=）是实验性功能，见[验证情况](#验证情况)。
- **国内镜像**（account.chefzc.dev）：启动时两个地址同时试，用先回应的那个（国内节点平均 52 毫秒，直连 72 毫秒）。
- **兑换码和插件包**：为之后的插件包打好底子。
- **1.5.1**：安装版能上传头像了；更新后不会被登出；Prism 第一次开始工作时不再卡住几秒（准备工作挪到了游戏渲染线程之外：测试程序前 8 秒出帧从 587 帧变成 1608 帧）。

## Prism Core

| | |
|---|---|
| **任意游戏** | 有 DLSS 的 DX12 游戏：Core 接手游戏自己的 DLSS 调用（画质最好、开销最小）。没有 DLSS 的 DX12 游戏：画面递交模式。DX9、DX10、DX11 和所有 32 位游戏：经 64 位辅助进程的画面桥。自动择优，DX12 优先。 |
| **档位** | 极致、旗舰、高画质、标准、流畅：决定网络多久跑一次、跑几遍、多大分辨率。每项设置都能手动调。 |
| **游戏内面板** | Home（手柄 View + Y）：帧时间条码、渲染帧率和显示帧率、1% 低帧、Prism 的 GPU 开销；所有设置实时改；分屏对比；目标帧率（帧率不够时自动降低网络分辨率）。帧生成画面、SDR、scRGB、HDR10 下都能用。 |
| **色彩风格** | 默认、高还原、高饱和、低饱和、浓烈，在游戏色调映射之前按场景光调色；另有"自定义"：66 项参数的调色台，游戏里和软件里都有。 |
| **守护** | 角色守护不让网络改肤色和发色；头发和细节保留对比度；每帧运行的档位不沿用旧结果，高速运动不拖影。 |
| **和其他 Mod 共存** | ReShade、OptiScaler 照常保留。D 加密游戏延迟加载，进游戏后第一次按 Home 才载入 Core，不改游戏文件、不碰加密。 |

地平线 6 性能测试，RTX 5090 Laptop，2560×1600，DLSS 画质 + 2 倍帧生成，同一时段连测：**Prism 旗舰 87 帧，极致 77 帧，RenoDX DLSS 5 8.5 是 67 帧**。同样每帧跑 2 遍网络，旗舰快约 30%。Core 的原理和全部设置见 [native/prism-core](native/prism-core/README.md)。

![CONTROL Resonant 同一帧经过 Prism 各种色彩风格](docs/screenshots/looks-control.png)

## 下载

[**PrismNR-1.5.1-x64-setup.exe**](https://github.com/ChefDavid0815/prism-nr/releases/tag/v1.5.1) · Windows 10 / 11 64 位 · 按用户安装，不需要管理员权限。旧版本直接覆盖安装，登录、设置和已启用的游戏都会保留。

[**PrismCore-1.5.1-win64.zip**](https://github.com/ChefDavid0815/prism-nr/releases/tag/v1.5.1) · 单独的 Prism Core 运行时，手动安装方法见 [native/prism-core](native/prism-core/README.md)。

更新的版本在官网：[prism.chefzc.dev](https://prism.chefzc.dev)。

安装包暂未做代码签名，第一次运行时 Windows 可能提示"已保护你的电脑"：点"更多信息"，再点"仍要运行"。

**需要**：NVIDIA RTX 50 系显卡，Windows 10 / 11 64 位。带反作弊的游戏会被拒绝安装：不要在在线游戏里使用注入类 Mod。

## 从源码构建

需要 Windows 10 / 11、Node.js 20+，神经渲染需要 RTX 50 显卡。Prism Core 用 MinGW-w64 g++ 构建（`native/prism-core/build.sh`）。

```bash
npm install
npm run dev        # Vite + Electron，实时刷新
npm run build      # 类型检查和生产构建
npm run test:core  # 对这台电脑做只读自检：显卡、扫描、分析、方案
```

## 验证情况

- 账号、社区、调色码、兑换码都在真实服务器上走通：验证码注册、三种登录方式、分享和导入、兑换以及兑换失败的各种情况。
- Prism Core 在 RTX 5090 Laptop 上实机运行：CONTROL Resonant、地平线 6（开 2 倍帧生成）；红色沙漠的 D 加密延迟加载。
- 画面桥（DX9、DX11，32 位和 64 位）用测试程序经真实辅助进程和网络走完全程；还没在实际游戏里测过。
- *副卡运行*（Offload=）：整条链路能跑通，画面和单卡逐像素一致；但每帧画面都要经内存在两块卡之间搬运并等待完成。在一台机器上实测，每帧多出约 18 毫秒（390 万像素），比单卡还慢，所以仍是实验性功能、默认关闭。还没在两块 RTX 卡上测过。

## 致谢与许可

Prism NR 以 MIT 许可开源。它不是 NVIDIA 官方产品，与 NVIDIA 无关联；DLSS 是 NVIDIA 的商标。安装包内置未经修改的 NVIDIA 签名版 DLSS 神经渲染运行库（`nvngx_dlssnr.dll`），它仍属于 NVIDIA，按 NVIDIA 的条款使用。Prism Core 不含 NVIDIA 代码，接口细节参考了 RenoDX DLSS5（MIT）和 neural-upstream（MIT）。[MinHook](https://github.com/TsudaKageyu/minhook) 为 BSD-2-Clause，JetBrains Mono 为 SIL OFL 1.1。1.2 之前的版本会从各自的发布页安装 ReShade、OptiScaler、DLSS5-Feeder、RenoDX 和 LumeniteFX；现在这些都不再随附，也不再下载。
