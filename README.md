# quiche-py

[![PyPI](https://img.shields.io/pypi/v/quiche-py)](https://pypi.org/project/quiche-py/)
[![image](https://img.shields.io/pypi/pyversions/quiche-py.svg)](https://pypi.python.org/pypi/quiche-py)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Actions status](https://github.com/shiguredo/quiche-py/workflows/CI/badge.svg)](https://github.com/shiguredo/quiche-py/actions)

## About Shiguredo's open source software

We will not respond to PRs or issues that have not been discussed on Discord. Also, Discord is only available in Japanese.

Please read <https://github.com/shiguredo/oss/blob/master/README.en.md> before use.

## 時雨堂のオープンソースソフトウェアについて

利用前に <https://github.com/shiguredo/oss> をお読みください。

## quiche-py について

[google/quiche](https://github.com/google/quiche) の Python バインディングです。

クライアント側の QUIC (RFC 9000)、HTTP/3 (RFC 9114)、WebTransport over HTTP/3 (draft-ietf-webtrans-http3) を利用できます。

C++ コアは I/O を持たない sans-I/O 設計です。UDP ソケットの所有と送受信は Python (asyncio) 側の責務です。

## 特徴

- I/O を持たない sans-I/O 設計
- UDP データグラムの送受信とイベントループは利用側 (asyncio) の責務
- 高レベル API: `AsyncQuicClient` / `AsyncHttp3Client` / `AsyncWebTransportClient`
- 低レベル API: `QuicClient` / `Http3Client` / `WebTransportClient`
- [nanobind](https://github.com/wjakob/nanobind) を利用しています
- [scikit-build-core](https://github.com/scikit-build/scikit-build-core) を利用してビルドしています
- free-threading (PEP 703) 対応

## 対象プロトコル

| プロトコル | 状態 |
| --- | --- |
| QUIC | 利用可能（`alpn` 必須） |
| HTTP/3 | 利用可能 |
| WebTransport over HTTP/3 | 利用可能 |
| WebTransport over HTTP/2 | 対象外 |

現状はクライアントのみです。サーバー API は提供していません。

## Python

- 3.14
- 3.14t

## プラットフォーム

- Ubuntu 24.04 LTS x86_64
- Ubuntu 24.04 LTS arm64
- macOS 26 arm64

## 使い方

```python
import asyncio

import quiche


async def main() -> None:
    client = quiche.AsyncHttp3Client(
        "example.com",
        443,
        server_name="example.com",
    )
    await client.connect()
    try:
        response = await client.get("/")
        print(response.status_code, response.body)
    finally:
        await client.close()


asyncio.run(main())
```

QUIC は `AsyncQuicClient`（`alpn` 必須）、WebTransport over HTTP/3 は `AsyncWebTransportClient` を利用します。

## ビルド前提

- [Bazelisk](https://github.com/bazelbuild/bazelisk) (`brew install bazelisk` 等)
- 初回は `deps.json` に従い google/quiche を取得するため、ネットワークが必要です

## リリースビルド

```bash
make wheel
```

## 開発ビルド

```bash
make develop
```

## テスト

```bash
make test
```

E2E テストの相手サーバーに [aioquic](https://github.com/aiortc/aioquic) を利用します。`make develop` / `make test` は dependency group `e2e` を含めて同期します。aioquic は free-threaded ビルド (3.14t) ではインストールできないため、該当環境では E2E の一部が skip されます。

## サポートについて

### Discord

- **サポートしません**
- アドバイスします
- フィードバック歓迎します

最新の状況などは Discord で共有しています。質問や相談も Discord でのみ受け付けています。

<https://discord.gg/shiguredo>

### バグ報告

Discord へお願いします。

## google/quiche ライセンス

<https://github.com/google/quiche/blob/main/LICENSE>

```text
// Copyright 2015 The Chromium Authors. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## ライセンス

Apache License 2.0

```text
Copyright 2026 Shiguredo Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```
