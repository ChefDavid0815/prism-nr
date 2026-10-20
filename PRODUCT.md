# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

（Windows 桌面应用，用 Electron 承载 Web 界面。）

## Stack

Electron + React + TypeScript + Vite（用户选择：先用 Electron，界面层以后可迁移到 Tauri）。原生插件为 ReShade FX 着色器。

## Users

- 重度 PC 玩家、高端显卡用户（RTX 5080/5090），追求当前最好的 DLSS 5 神经渲染画质。
- 中低端 RTX 50 用户（RTX 5060/5070），希望在帧率损失尽量小的前提下体验神经渲染。
- 使用场景：在启动游戏前打开软件，选游戏、选方案、一键启用；进游戏后用快捷键微调或对比。

## Product Purpose

Prism NR 是一个 DLSS 5 神经渲染管理器：自动检测已安装的游戏和显卡，按硬件推荐优化方案，一键部署神经渲染及配套插件（画质、性能、去 AI 味），并能完整回滚。成功的标准是：玩家不需要懂 ReShade、DLL、预处理宏，也能稳定地拿到比现有社区工具更好、更流畅的效果。

## Positioning

- 不自研神经渲染模型，NVIDIA 的模型由用户本机合法来源提供；Prism 的价值在编排、性能调度和画质守护。
- 集成多种插件，每个插件写清楚用途、开销和适用显卡。
- 自带 Prism 原生插件（保真守护、时域稳定、细节还原、色调锚定、A/B 对比），专门解决现有方案的 AI 味、闪烁、色调漂移问题。
- 对效果预期诚实：按显卡给出真实预期，不宣称支持 AMD 或"零损失"。

## Operating Context

- 依赖 ReShade（带 add-on 支持版）、社区神经渲染插件（RenoDX DLSS 5 add-on / Deep Fried Chicken）、DLSS5-Feeder、OptiScaler 等第三方组件。
- 官方 DLSS 5 神经渲染仅支持 RTX 50 系，RTX 40 官方支持未定。
- 带反作弊的网络游戏默认拦截。

## Capabilities and Constraints

- 开源免费（MIT），可集成开源社区组件，但遵守各自协议（GPL 组件需随附源码义务）。
- 不打包、不分发 NVIDIA 二进制文件（nvngx_dlssnr.dll 等），只定位并校验用户本机已有的文件。
- 所有对游戏目录的改动都必须可回滚（事务式安装 + 备份清单）。
- 开发机：RTX 5090 Laptop GPU 24GB，驱动 617.42，Windows 11。

## Brand Commitments

- 名称：Prism NR（Prism Neural Rendering）。中文界面优先。
- 产品名中不直接使用 NVIDIA 商标作为品牌名，用"for DLSS 5"这类描述性写法。

## Evidence on Hand

无真实用户评价、跑分或案例。不得在界面中编造性能数字或用户评价；引用的社区数据需标注来源。

## Product Principles

1. 默认就好用：自动检测、自动选路线、预设开箱即用，高级参数收起来。
2. 性能优先：优先把神经渲染放到低分辨率阶段执行，按显卡分档。
3. 尊重原作：神经渲染是增强，不是替换；默认限制改动幅度，保护 UI 和人脸。
4. 永远可撤销：任何改动都能一键还原。
5. 说实话：讲清楚每个插件做什么、花多少性能、有什么风险。
