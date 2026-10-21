# DLSS 5 神经渲染技术研究报告

2026-10-21 · 为 Prism 1.1 做的技术调研。所有数字都注明来源；标"估算"的是我们根据公开数据推算的，不是实测。

## 一句话结论

DLSS 5 神经渲染（下文简称 NR）的成本 **99% 在 NVIDIA 的网络本身**，而且与处理的像素数基本成正比。社区已经证明调内核、调精度都没有收益，能动的只有三件事：**少算像素、少算帧、把算力挪走或叠起来**。社区在这三条路上都做了，但留下了几个没解决的硬伤：隔帧导致帧时间锯齿（帧生成下卡顿）、降分辨率固定丢细节、整屏一刀切地分配算力、以及大量参数要用户手调。这些就是 Prism 1.1 的突破口。

---

## 1. NR 是什么

| 项目 | 内容 | 来源 |
|---|---|---|
| 模型 | 一步（one-step）像素空间扩散模型，3D 引导的神经渲染 | NVIDIA ADLR 研究页 |
| 输入 | 当前帧颜色、引擎运动矢量、携带的时间状态、艺术方向参数；实际调用还会传深度 | ADLR；RenoDX 源码 |
| 性质 | 因果、确定性、逐帧（一帧进一帧出），按帧计算预算设计 | ADLR；NVIDIA 开发者博客 |
| 规模 | 10 月新模型约 1.18 亿参数（缩小 24%），4K 显存少 20%，GPU 开销减半；比 3 月发布时快 10 倍 | NVIDIA 10/13 新闻 |
| 精度 | FP8 算术（RDNA 4 原生支持、RDNA 3 需软件回退的原因） | TechTimes 对 AMD 移植的报道 |
| 运行时 | `nvngx_dlssnr.dll`，约 158 MB，其中 `.rsrc` 约 147 MB 是权重；`.data` 里 15 个 fatbin 带 PTX；由驱动的 `_nvngx.dll` 加载 | neural-upstream FINDINGS；TechTimes |
| 内部结构 | 内核以地址运算为主（IMAD 占静态指令 19%），张量指令只占 4.2%；包含 attention 内核 | neural-upstream FINDINGS（RTX 4070 Ti 反汇编） |
| 官方支持 | 仅 RTX 50 系（含 RTX Spark 笔记本）；NVIDIA 表示调优后会扩展到 RTX 40，未给日期；对 RTX 30/20 无承诺 | NVIDIA；heise |
| SDK 状态 | **未进入公开 SDK**：NVIDIA/DLSS 310.9.16 的头文件里没有 NR，Feature 18 在公开头里只是 `Reserved18`。社区接口全部来自逆向 | 我们核对 NVIDIA/DLSS 仓库 |

开发者侧（游戏原生集成）另有模型选择、Structure / Tone 强度、语义 AI 遮罩、引擎级遮罩等控制（NVIDIA 开发者博客）。社区注入方案拿不到引擎的语义遮罩，这是注入式方案的天然短板。

## 2. 调用契约（从 RenoDX 源码读出）

来源：`Andarael/RenoDX-DLSS5-Generic`（MIT，v7.5.0-rc5），`dlssnr.hpp` 第 4914–5215 行。

**创建**（`NVSDK_NGX_Feature_Reserved18`）：
- `Width/Height` 传输入尺寸，`OutWidth/OutHeight` 传输出尺寸；私有键 `DLSSNR.Width/Height` 描述原生工作面。
- `DLSSNR.ScalingRatio` 固定 1.0，并注册 `DLSSNRComputeScalingRatioCallback` 返回 1.0。**签名运行时直接拒绝低分辨率颜色输入**，所以降分辨率的做法是：按工作分辨率 1:1 创建一个更小的 feature，再由外部着色器把结果放大回去。neural-upstream 也实测 ScalingRatio < 1 无效。
- HDR 时加 `IsHDR | AutoExposure` 标志，曝光键置 1（运行时 310.8 实际不读曝光和抖动键，RenoDX 用字符串探测确认）。
- 加载门禁：NGX 只在调用模块路径含 `nvngx.dll` 时允许创建 feature，否则返回 `0xBAD00002`（neural-upstream 因此把插件命名为 `nvngx.dll.addon64`）。

**求值**：
- 资源：`DLSSNR.Color`（归一化后的代理图）、`DLSSNR.Output`、`DLSSNR.MVec`、`DLSSNR.Depth`。
- 子矩形：Color / Depth / MVec / Output 各有 `SubrectBase/Width/Height`。RenoDX 用 MVec / Depth 子矩形对齐游戏的引导缓冲。
- `DLSSNR.MVecScaleX/Y`（运行时会读，已实测生效）、`DLSSNR.DepthInverted`、`DLSSNR.Reset`、`DLSSNR.Intensity`，以及风格、Local Tone、Local Structure、Skin Structure 等键。
- **时间历史**：每个 feature 槽自带历史。多 pass 时第 2 遍以后要么每帧 Reset（无状态，会闪），要么链式保留历史。NVIDIA 文档警告每帧 Reset 会导致时域闪烁。
- **NGX 可以在计算队列上创建并求值 feature**（Skynizz 实测），这是异步方案的前提。

**颜色**：网络要的是有界的显示参考图。RenoDX 的 v6 编码先按工作分辨率做面积加权缩小，再用色相保持的肩部曲线压缩、暗部抬升（FP8 在暗部精度不够），输出后再逆变换。neural-upstream 把网络的贡献读成逐像素亮度增益乘回原图，保留 HDR 范围和原有色相饱和度。

## 3. 成本模型

成本几乎与输出像素数成正比，再乘以 GPU 的张量吞吐（TechSpot）。

| GPU | 4K | 1440p | 折合每百万像素 | 来源 |
|---|---:|---:|---:|---|
| RTX 5090（台式） | 7.9 ms | 3.6 ms | 约 0.95 ms | TechSpot，旧模型 |
| RTX 5070 | 22.1 ms | 10.2 ms | 约 2.7 ms | TechSpot，旧模型 |
| RTX 5060 | 37.8 ms | 16.9 ms | 约 4.6 ms | TechSpot，旧模型 |
| RTX 5070 Ti | 约 17 ms | — | 约 2.0 ms | Nexus Mods 用户实测 |
| RTX 4070 Ti（社区运行时） | — | 3.27 ms @ 1280×720 | 约 3.5 ms | neural-upstream GPU 计时 |
| RTX 5090 Laptop（我们） | — | — | 约 0.87 ms / 遍 | 我们在 CONTROL 的实测（2560×1600，基础帧 74.5 → 48.7 FPS，2 遍） |

10 月新模型官方称 GPU 开销减半；社区在 RTX 40 上测到 8.8 → 5.4 ms。**估算**：新模型下 RTX 5060 约 2.3 ms/百万像素，1440p 整屏一遍约 8.5 ms，仍占 60 FPS 帧预算（16.7 ms）的一半。

**调内核没用**：neural-upstream 在 Ada 上试了 L1 绕过、寄存器上限 128/200、cp.async 改普通读写、FP16 替代 FP8，全部在噪声内或更差。NVIDIA 的 `.maxnreg 168` 已经是最优点。

**结论：唯一的杠杆是调度**——每帧让网络处理多少像素、哪些像素、哪几帧、在哪个队列或哪张卡上。

## 4. 社区方案全景

| 方案 | 核心手段 | 实测效果 | 暴露的问题 |
|---|---|---|---|
| **RenoDX DLSS5**（MIT，我们 0.1 用的） | DLSS 输出后注入；工作分辨率 33–100% + 联合双边边缘感知上采样（4×4 邻域、亮度引导，Kopf 2007）；"look" 阶段在 log 比值编辑域调色；多 pass | CONTROL：2 遍 148.9 → 97.4 FPS（我们实测） | 每帧都跑满；50% 分辨率时细节固定损失；没有隔帧复用 |
| **neural-upstream**（MIT） | 前置超分（在渲染分辨率上跑）；按 DLSS 抖动锚定节奏；跳过的帧沿 MV 重投影"效果"（差值），按深度剔除遮挡 | 4070 Ti 720p 网络 3.27 ms | **隔帧使渲染间隔在 8.9 / 13.9 ms 间交替，帧生成无法平滑，出现卡顿和闪光**；按"编辑年龄"修正重投影两次都更差 |
| **Skynizz OptiScaler-DLSS5**（GPL-3，社区最强） | 前置超分；时域编辑缓存（log 比值、低频/高频分离、深度+颜色+指纹校验、soft refresh）；自适应节奏（静止 8 帧一次、运动时 2 帧）；ms 预算；异步计算队列；光照突变平滑；区域遮罩 | TLOU 4070 1440p：83.9 → 39.2（原版）→ 77.3 FPS（x8 预设）；CONTROL：34.4 → 55.3 FPS | x8 时模型帧约 9 ms、其他帧 2.2 ms，**帧时间仍有锯齿**（p99 摆动 1.8 ms）；x8 细节只剩 x1.04（对比 x1.18）；无 MV 的物体（纸片、手）在模型帧闪一下，需要补丁；隔帧下"效果保留"随间隔下降（x8 平移时 89%） |
| **MGPU Bridge** | NR 是终端阶段，整帧交给第二张卡算 | 双 5060 Ti 1080p DLAA：44 → 67–70 FPS（NR 成本从 36% 降到 0%） | 需要两张 N 卡、显示接线讲究 |
| **NeuralScreen** | 抓整个桌面跑 NR | 5070 Ti 验证 | 研究性质，捆绑运行时 |
| **DLSS-NR on AMD** | 从 `nvngx_dlssnr.dll` 取权重，自写 HIP 推理引擎 | RX 9070 XT 1080p 90–100 FPS（开发者自测） | 使用 NVIDIA 权重、法律风险高 |
| **社区 RTX 40/30 运行时** | 解出 PTX 重编译 `sm_89`、替换 TMA/mbarrier 指令、打补丁跳过架构分发 | 40 系可跑 | 修改并分发 NVIDIA 二进制，无签名 |

## 5. 已经被证明有效的手段

1. **工作分辨率 + 边缘感知放大**：成本按面积下降；联合双边放大把 4K 编辑的 RMSE 从 0.248 降到 0.172 档（RenoDX 注释中的测量）。
2. **前置超分**：在渲染分辨率上跑，像素少约 3 倍，DLSS 的时域累积还能平滑网络的新增内容。
3. **编辑缓存**：只缓存"网络改了什么"（`log2(编辑后/原图)`），不缓存画面；比值比差值更能经受住光照变化。
4. **异步计算**：模型帧放到自己的计算队列，与下一帧并行；x8 时帧时间锯齿减半、1% 低帧 +14%；3 帧间隔时没有收益。
5. **双卡分担**：把网络整个挪走，收益最大，但门槛最高。

## 6. 仍未解决的问题（Prism 的机会）

1. **隔帧 = 帧时间锯齿**。neural-upstream 和 Skynizz 都量到了，现有缓解办法（把模型帧做轻、异步）只是减小锯齿，没消掉。帧生成越高倍越明显。
2. **降分辨率的细节损失是永久的**。现在的放大每帧都从同一个低分辨率网格出发，丢掉的高频再也回不来。
3. **算力平均分配**。天空、墙面和人脸拿同样的分辨率。人眼最在意的是脸和视线中心，这里恰恰最需要全分辨率。
4. **跳帧时模型的时间状态被打断**。模型的历史期待连续帧；隔帧喂它会让它自己的时域稳定失效，社区用"累积运动"部分补救。
5. **参数太多、全靠手调**。节奏、比例、阈值、预算，每个工具几十个开关，没有人按"你的显卡 + 你的游戏 + 你要的帧率"自动算好。
6. **角色形象漂移**。我们的宣传片已经拍到：强度 1.0 下肤色发橙、红发变暗。

## 7. 边界：我们做什么，不做什么

- **不分发、不修改 NVIDIA 的任何二进制**，不提取权重另建推理引擎。这条路（AMD 移植、40 系重编译）技术上可行，但违反 NVIDIA 的许可，也会让 Prism 本身陷入风险。
- 只使用用户本机已有、NVIDIA 签名有效的运行时。
- **"打破只能在高端电脑上运行"在 Prism 里的含义**：在官方支持的显卡上（RTX 50 全系，包括 5050、5060 和笔记本），把神经渲染的成本压到能流畅玩的程度。NVIDIA 开放 RTX 40 后，同一套调度架构不用改就能受益，而且越弱的卡收益越大。

## 来源

- [NVIDIA ADLR：DLSS 5 Generative Neural Rendering](https://research.nvidia.com/labs/adlr/DLSS5/)
- [NVIDIA 开发者博客：DLSS 5 与 3D 引导神经渲染](https://developer.nvidia.com/blog/whats-new-for-game-developers-dlss-5-with-3d-guided-neural-rendering-nvidia-ace-updates-and-new-rtx-kit-capabilities/)
- [NVIDIA：更快的 DLSS 5 模型（10 月 13 日）](https://www.nvidia.com/en-us/geforce/news/dlss-5-faster-ai-model-hogwarts-legacy-rtx-spark-laptops/)
- [heise：NVIDIA 将把神经渲染带到 RTX 40](https://www.heise.de/en/news/DLSS-5-Nvidia-is-finally-bringing-neural-rendering-to-RTX-40-cards-11441795.html)
- [TechSpot：DLSS 5 真实性能测试](https://www.techspot.com/article/3170-real-dlss-5-performance/) · [TechSpot：新模型性能](https://www.techspot.com/article/3190-dlss-5-performance-v2/)
- [VideoCardz：新模型在 RTX 40 上运行](https://videocardz.com/newz/nvidias-faster-dlss-5-model-already-runs-on-rtx-40-hardware-within-hours-of-release)
- [TechTimes：DLSS 5 神经渲染运行在 AMD Radeon 上](https://www.techtimes.com/articles/329228/20261020/dlss-5-neural-rendering-now-runs-three-generations-amd-radeon-gpus.htm)
- [Andarael/RenoDX-DLSS5-Generic（源码）](https://github.com/Andarael/RenoDX-DLSS5-Generic)
- [matiasLombo/neural-upstream（含 FINDINGS.md）](https://github.com/matiasLombo/neural-upstream)
- [Skynizz/optiscaler-dlss5（含 edit-cache 设计文档）](https://github.com/Skynizz/optiscaler-dlss5)
- [maohgad-web/Neural-coprocessor（MGPU Bridge）](https://github.com/maohgad-web/Neural-coprocessor)
- [wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
- [perseval-BLR/NeuralScreen](https://github.com/perseval-BLR/NeuralScreen)
- [An0sTheGreat/DLAssAss-5-Tool](https://github.com/An0sTheGreat/DLAssAss-5-Tool)
- [Nexus Mods：Stellar Blade DLSS 5 设置指南](https://www.nexusmods.com/stellarblade/mods/3706)
- [Nehab et al. 2007：Reverse Reprojection Caching](https://diglib.eg.org/handle/10.2312/EGGH.EGGH07.025-036)
- [NVIDIA/DLSS SDK 仓库](https://github.com/NVIDIA/DLSS)
