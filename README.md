# epollo

[![CI](https://github.com/MAX959954/epollo/actions/workflows/ci.yml/badge.svg)](https://github.com/MAX959954/epollo/actions/workflows/ci.yml)

HTTP/1.1 + TLS сервер и reverse proxy на C++20 без сетевых фреймворков.
Основная платформа — Linux (`epoll`), Windows (`WSAPoll`) поддерживается для разработки.

> Статус: ранняя стадия (Фаза 1 — каркас проекта). План — в [docs/PLAN.md](docs/PLAN.md).

## Сборка

Нужны CMake ≥ 3.25, Ninja и [vcpkg](https://github.com/microsoft/vcpkg)
с переменной окружения `VCPKG_ROOT`. Зависимости (OpenSSL, GoogleTest) vcpkg ставит сам.

### Linux

```sh
cmake --preset linux-debug      # или linux-release, linux-asan
cmake --build --preset linux-debug
ctest --preset linux-debug
```

### Windows (MSVC)

Из **Developer PowerShell for VS**:

```powershell
cmake --preset windows-debug    # или windows-release
cmake --build --preset windows-debug
ctest --preset windows-debug
```

## Пресеты

| Пресет | ОС | Что |
|---|---|---|
| `linux-debug` | Linux | Отладочная сборка |
| `linux-asan` | Linux | Debug + AddressSanitizer + UBSan |
| `linux-release` | Linux | Оптимизированная сборка (`NDEBUG`) |
| `windows-debug` | Windows | Отладочная сборка MSVC |
| `windows-release` | Windows | Оптимизированная сборка MSVC |

Предупреждения считаются ошибками (`-Werror` / `/WX`); отключить: `-DEPOLLO_WERROR=OFF`.

## Стиль

Форматирование — `.clang-format`, статический анализ — `.clang-tidy`.
