# CI ワークフローと開発フックを堅牢化する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/change-harden-ci-workflows
- Polished: {YYYY-MM-DD}

## 目的

GitHub Actions ワークフローの権限とサプライチェーン、開発フックの実行項目を改善する。

## 現状

- `.github/workflows/wheel.yml` の `permissions: contents: write` がワークフローレベルで全ジョブ（build_ubuntu / build_macos を含む）に付与されている。write が必要なのは create-release のみで、ci.yml は `contents: read` に適切に絞られている（両者の水準が揃っていない）
- `ci.yml` / `wheel.yml` で bazelisk バイナリを GitHub releases の `latest` で取得しており、bazelisk 自体のバージョンが固定されていない（bazel 本体は QUICHE の .bazelversion で固定されるため実害は限定的だが、再現性の観点で問題）
- `prek.toml` に pytest フックが無い（ruff format / ruff check / ty check / tombi はある。時雨堂 Python 規約は「最低限 ruff format / ruff check / ty check / pytest と tombi の lint / format をフックすること」を求める）
- `wheel.yml` の artifact アップロード（`path: wheelhouse/*.whl`）とダウンロード（`path: wheelhouse/`）でディレクトリのネストが冗長

## 設計方針

- ワークフローの権限をジョブレベルで最小化する
- bazelisk の取得をコミットハッシュ固定（または特定バージョン）にする
- pytest フックの追加可否と実行コスト（E2E を含むため）を検討して決定する

## 完了条件

- wheel.yml のビルドジョブが `contents: read` で動作すること
- bazelisk のバージョンが固定されること
- pytest フックの追加または除外理由の明文化がなされること

## 解決方法

- `wheel.yml` の `permissions` をジョブレベルに移動し、create-release のみ write にする
- bazelisk の取得 URL を固定バージョン（例: v1.x.x のリリースアセット）にする
- `prek.toml` に pytest フックを追加するか、E2E 実行コストを理由に除外する旨をコメントで明記する
- artifact のパス構成を整理する
