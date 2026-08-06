# CinderX

<!-- hy-mt2-i18n:start -->
[English](./README.md) | **中文** | [日本語](./README_ja.md) | [Español](./README_es.md)
<!-- hy-mt2-i18n:end -->


[![PyPI - 版本](https://img.shields.io/pypi/v/cinderx.svg)](https://pypi.org/pypi/cinderx/)

![CinderX 徽标——由小写的“cinderx”组成，其中字母 i 上的点被设计成一小团火焰，旁边还有一个风格化的橙色 X](assets/png/logo.png)

CinderX 是一种能够提升 Python 运行时性能的 Python 扩展模块。

## 状态

CinderX 目前正处于积极开发阶段。Meta 已在其生产环境中将该工具用于 Instagram Django 服务等场景。对于外部用户而言，它仍属于**实验性**产品。新的版本会每周发布到 PyPI 上。

## 功能特性

- **JIT编译器**——将Python字节码即时编译为原生机器码  
- **静态Python**——一种更严格的Python形式/子集，用于实现类型安全与优化

该代码库还包含其他功能，比如并行垃圾收集器以及更轻量级的 Python 解释器帧实现。不过这些功能目前还不兼容原生 CPython 运行时。

## 系统要求

- Python 3.14
- GCC 13+ 或 Clang 18+

|         |        Linux       |        macOS       |       Windows      |
| ------- | ------------------ | ------------------ | ------------------ |
|  x86-64 | :white_check_mark: |         :x:        | :white_check_mark: |
| aarch64 | :white_check_mark: | :white_check_mark: |         :x:        |

## 安装

```bash
pip install cinderx
```

## 使用 JIT

开始使用 JIT 的推荐方式是执行以下操作：

```python
import cinderx.jit

cinderx.jit.auto()
```

这将配置 CinderX 扩展，使其自动将 Python 函数编译为机器码。它会追踪那些被频繁调用的函数，并自动编译最常用的那些函数。

如需了解更多详细信息，请参阅[JIT文档](https://facebookincubator.github.io/cinderx/jit)，或浏览完整的[CinderX文档网站](https://facebookincubator.github.io/cinderx/)。

## CinderX 与 Cinder 的对比

[Cinder](https://github.com/facebookincubator/cinder) 是 Meta 开发的 CPython 运行时的一个分支。它包含了运行时优化功能（例如 JIT），并且是专门为 Instagram 的 Django 代码库设计的。在 Python 3.10 版本中，Meta 决定将其转变为一个 Python 扩展，以此提升与更新版 Python 的兼容性。这个扩展如今被称为 CinderX（“X”代表“extension”即扩展）。

从历史情况来看，在 Python 3.10 到 3.12 版本期间，CinderX 需要依赖 Meta 所开发的 Python 运行时分支的补丁才能运行。而 Python 3.14 则是 CinderX 首次支持的原生 CPython 版本。

## 许可证

CinderX 采用 MIT 许可协议，详情请参见 LICENSE 文件。

## 使用条款

https://opensource.fb.com/legal/terms

## 隐私政策

https://opensource.fb.com/legal/privacy政策

# 严格约束
1. **结构锁定**：绝对保持原有的 Markdown 数据结构、缩进、标题层级、表格、链接、URL、徽章、代码块和行内代码完全不变。
2. **选择性翻译**：仅翻译面向用户展示的可见自然语言内容。
3. **禁止修改**：**严禁**翻译或更改代码标签、键名、变量占位符（如 {{var}}、${var}、%s、%d 等）、命令示例、文件路径、项目名、API 名、包名、模型名、标识符和代码符号；除非背景信息中已经给出对应译名。
4. 术语、风格、专有名词的译法要与所给背景信息保持一致。

版权所有 © 2025 Meta Platforms, Inc.
