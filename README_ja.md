# CinderX

<!-- hy-mt2-i18n:start -->
[English](./README.md) | [中文](./README_zh-CN.md) | **日本語** | [Español](./README_es.md)
<!-- hy-mt2-i18n:end -->


[![PyPI - バージョン](https://img.shields.io/pypi/v/cinderx.svg)](https://pypi.org/pypi/cinderx/)

![CinderXのロゴ。小文字の「cinderx」で、iの中の点が小さな炎の形をしており、オレンジ色のスタイリッシュなxが描かれています。](assets/png/logo.png)

CinderXは、Pythonランタイムのパフォーマンスを向上させるPython拡張機能です。

## ステータス

CinderXは現在も積極的に開発が進められています。MetaではInstagram Djangoサービスなどの用途で本番環境で利用されていますが、外部ユーザー向けには**実験的な**ものです。新しいバージョンは毎週PyPIに公開されています。

## 機能概要

- **JITコンパイラ** – Pythonバイトコードをネイティブなマシンコードに
  ジャストインタイムでコンパイルする機能
- **Static Python** – 型の安全性と最適化のための、より厳格な形式または
  Pythonのサブセット

このコードベースには、並列ガベージコレクターやより軽量なPythonインタプリタフレームの実装といったその他の機能も含まれています。ただし、これらの機能は現時点では標準のCPythonランタイムとは互換性がありません。

## 要求事項

- Python 3.14
- GCC 13+ または Clang 18+

|         |        Linux       |        macOS       |       Windows      |
| ------- | ------------------ | ------------------ | ------------------ |
|  x86-64 | :white_check_mark: |         :x:        | :white_check_mark: |
| aarch64 | :white_check_mark: | :white_check_mark: |         :x:        |

## インストール

```bash
pip install cinderx
```

## JITの利用方法

JITの利用を開始するための推奨方法は、次のように行うことです：

```python
import cinderx.jit

cinderx.jit.auto()
```

これにより、CinderX拡張機能がPython関数を自動的にマシンコードにコンパイルするよう設定されます。頻繁に呼び出される関数を追跡し、最も頻繁に使用される関数だけを自動的にコンパイルします。

詳細については、[JITのドキュメント](https://facebookincubator.github.io/cinderx/jit)をご覧いただくか、完全な[CinderXドキュメントサイト](https://facebookincubator.github.io/cinderx/)を閲覧してください。

## CinderXとCinderの違い

[Cinder](https://github.com/facebookincubator/cinder)は、Metaで開発されたCPythonランタイムのフォークです。これにはランタイム最適化機能（例：JIT）が含まれており、InstagramのDjangoコードベース向けに特別に設計されていました。Python 3.10では、より新しいバージョンのPythonとの互換性を向上させるため、MetaはこれをPython拡張機能に変更することにしました。この拡張機能は現在、CinderXとして知られています（「X」とは「extension」の略です）。

過去、Python 3.10から3.12のバージョンでは、CinderXはMetaが作成したPythonランタイムのフォークに対するパッチに依存していました。Python 3.14は、CinderXがサポートする標準CPythonとしては初のバージョンです。

## ライセンス

CinderXはMITライセンスで提供されており、LICENSEファイルをご覧ください。

## 利用規約

https://opensource.fb.com/legal/terms

## プライバシーポリシー

https://opensource.fb.com/legal/privacy-policy

---

著作権 © 2025 Meta Platforms, Inc.
