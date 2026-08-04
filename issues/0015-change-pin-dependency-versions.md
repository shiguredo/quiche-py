# 依存ライブラリのバージョン指定を固定する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/change-pin-dependency-versions
- Polished: {YYYY-MM-DD}

## 目的

pyproject.toml の依存ライブラリにバージョン指定を付け、再解決時の破壊的変更の吸い込みを防ぐ。

## 現状

`pyproject.toml` の依存がすべてバージョン指定なしで最新に流れる:

- dev グループ: `ruff` / `ty` / `hypothesis` / `pytest` / `pytest-asyncio`
- e2e グループ: `aioquic`
- build-system: `nanobind` / `scikit-build-core`（free-threading 対応のため nanobind は 2.5 以上の利用が時雨堂 Python 規約で求められているが、その下限すら指定されていない）

uv.lock は現状を固定するが、`uv sync` の再解決や CI のキャッシュクリアで最新版を吸い込む余地がある。

## 設計方針

時雨堂 Python 規約に従い、マイナーバージョンまでの固定（`~=X.Y` 形式）を適用する。build-system は最低限 `nanobind>=2.5` の下限を入れる。

## 完了条件

- 全依存にバージョン指定が付き、lock 再解決でマイナー未満のバージョンしか入らないこと

## 解決方法

- `pyproject.toml` の dependency-groups と build-system requires にバージョン指定を追加する
- `uv lock` で `uv.lock` を再生成し、テスト・型チェック・ビルドが通ることを確認する
