# take_response の nb::cast 失敗時にレスポンスがリークする

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-take-response-leak
- Polished: {YYYY-MM-DD}

## 目的

`Http3Client.take_response()` で Python インスタンス生成に失敗した場合に、C 側リソース（`quiche_py_http3_response`）を確実に解放する。

## 現状

`src/bindings/module.cpp` の `PyHttp3Client::take_response()` は:

```cpp
return nb::cast(new PyHttp3Response(response), nb::rv_policy::take_ownership);
```

`nb::cast` による Python インスタンス生成が失敗した場合（MemoryError 等の OOM 時）、`new` した `PyHttp3Response` と `quiche_py_http3_response` handle は破棄されない。デストラクタ（`PyHttp3Response` のデストラクタ → `h3_response_destroy`）が唯一の解放経路だが、コンストラクタ失敗時には呼ばれない。

## 設計方針

`new` でなく RAII（`std::unique_ptr` 等）で所有し、`nb::cast` 失敗時に確実に解放する。

## 完了条件

- `nb::cast` が失敗しても `quiche_py_http3_response` がリークしないこと

## 解決方法

- `PyHttp3Response` を `std::unique_ptr` で保持し、`nb::cast` に生ポインタを渡す前に所有権を移す形にする
- または `nb::cast` 失敗時に `h3_response_destroy(response)` を呼ぶ
