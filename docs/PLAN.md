# epollo — план реализации

Кроссплатформенный HTTP/1.1 + TLS сервер на современном C++ (C++20) без сетевых фреймворков.
Linux: `epoll`. Windows: `WSAPoll` (основной backend), `IOCP` (финальный этап).

---

## 0. Ключевое архитектурное решение (прочитать первым)

`epoll` и `IOCP` работают по **разным моделям**:

| | epoll / WSAPoll | IOCP |
|---|---|---|
| Модель | **readiness** (reactor): «сокет готов, читай сам» | **completion** (proactor): «вот буфер, сообщи, когда прочитаешь» |
| Кто делает `recv` | твой код, после уведомления | ядро, в твой буфер, заранее отданный |
| Буфер | нужен только в момент чтения | должен жить, пока операция не завершится |

Поэтому абстракция «один интерфейс на оба» строится так:

1. **Ядро проекта — reactor-интерфейс `Poller`** (readiness). Backends: `EpollPoller` (Linux) и
   `WsaPollPoller` (Windows). Они ложатся на один интерфейс почти один в один, поэтому проект
   заработает на обеих ОС быстро.
2. **IOCP — отдельный этап (Фаза 10)**. Над reactor-ом строится тонкий слой
   с методами `async_read / async_write` + колбэк по завершению. На Linux он реализуется
   через epoll (прочитал при готовности → вызвал колбэк), на Windows — нативно через IOCP.
   Так устроен Boost.Asio, и это отличная тема для собеседования.

Почему не сразу IOCP: если начать с него, придётся одновременно разбираться с OVERLAPPED,
временем жизни буферов, AcceptEx и отменой операций — легко застрять и ничего не довести.

**TLS не привязан к сокету**: OpenSSL используется через *memory BIO* — шифруем/дешифруем
байты в памяти, а читает/пишет сокет наш event loop. Это работает одинаково для любого backend.

---

## 1. Стек и инструменты

| Что | Выбор | Зачем |
|---|---|---|
| Язык | C++20 | `std::span`, `std::string_view`, concepts, `std::jthread` |
| Сборка | CMake ≥ 3.25 + `CMakePresets.json` | Одинаково в VS, VS Code, CLion, CI |
| Зависимости | vcpkg (manifest mode, `vcpkg.json`) | OpenSSL на Windows без боли |
| TLS | OpenSSL 3.x | Стандарт индустрии |
| Тесты | GoogleTest | Самый частый в вакансиях |
| Фаззинг | libFuzzer (clang, Linux) | Парсер HTTP — идеальная цель |
| Качество | clang-format, clang-tidy, ASan/UBSan/TSan (Linux), `/W4 /WX` (MSVC) | |
| CI | GitHub Actions: ubuntu (gcc + clang), windows (MSVC) | Доказывает кроссплатформенность |
| Бенчмарк | `wrk` (Linux), `bombardier` (Windows) | Цифры в README |

Сторонних сетевых/HTTP библиотек **нет** — только OpenSSL и GoogleTest.

---

## 2. Структура репозитория

```
epollo/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ vcpkg.json
├─ .clang-format / .clang-tidy / .gitignore
├─ README.md
├─ docs/
│  ├─ PLAN.md               ← этот файл
│  └─ ARCHITECTURE.md       ← схема потоков/классов (по мере готовности)
├─ include/epollo/          ← публичные заголовки
│  ├─ net/    socket.hpp, address.hpp, error.hpp
│  ├─ io/     poller.hpp, event_loop.hpp, timer_queue.hpp, buffer.hpp
│  ├─ http/   request.hpp, response.hpp, parser.hpp, router.hpp
│  ├─ tls/    tls_context.hpp, tls_session.hpp
│  └─ server/ server.hpp, connection.hpp, config.hpp
├─ src/
│  ├─ net/ io/ http/ tls/ server/       ← платформонезависимый код
│  └─ platform/
│     ├─ linux/   epoll_poller.cpp, wakeup_eventfd.cpp
│     └─ windows/ wsapoll_poller.cpp, wakeup_socketpair.cpp, wsa_init.cpp, iocp_*.cpp
├─ apps/epollo/main.cpp     ← исполняемый файл (CLI)
├─ tests/                   ← unit + integration
├─ fuzz/                    ← fuzz_http_parser.cpp
├─ bench/                   ← скрипты wrk/bombardier, результаты
└─ www/                     ← тестовая статика
```

Правило: `#ifdef _WIN32` разрешён **только** в `src/platform/` и `net/socket.hpp`.
Остальной код не знает, на какой он ОС. CMake подключает нужную папку `platform/*`.

---

## 3. Фазы

Каждая фаза заканчивается рабочим состоянием, зелёным CI и коммитом/тегом.
Оценки — при ~10–15 часах в неделю.

### Фаза 1. Каркас проекта (≈ 3–4 дня)

- `CMakeLists.txt`: библиотека `epollo_core` + exe `epollo` + `epollo_tests`.
- Пресеты: `linux-debug`, `linux-asan`, `windows-msvc-debug`, `windows-msvc-release`.
- Предупреждения: `-Wall -Wextra -Wpedantic -Werror` / `/W4 /WX /permissive-`.
- `vcpkg.json` с `openssl`, `gtest`.
- CI-матрица: ubuntu-gcc, ubuntu-clang (+ASan/UBSan), windows-msvc.
- Тривиальный тест, чтобы CI был зелёным.
- `.gitignore`: `.vs/`, `build/`, `out/`, `vcpkg_installed/`.

**Готово, когда:** `cmake --preset ... && cmake --build && ctest` проходит на обеих ОС в CI.

### Фаза 2. Платформенный слой сокетов (≈ 1 неделя)

- `native_socket_t`: `int` на Linux, `SOCKET` на Windows; константа `invalid_socket`.
- **`Socket` — RAII, move-only**: деструктор вызывает `close` / `closesocket`.
- `WsaInit` — RAII для `WSAStartup`/`WSACleanup` (только Windows).
- Ошибки: `last_error()` → `std::error_code` (`errno` vs `WSAGetLastError`), отдельная
  проверка «would block» (`EAGAIN/EWOULDBLOCK` vs `WSAEWOULDBLOCK`).
- Операции: `bind`, `listen`, `accept`, `recv`, `send`, `set_nonblocking`
  (`fcntl` vs `ioctlsocket(FIONBIO)`), `SO_REUSEADDR`, `TCP_NODELAY`.
- `SIGPIPE`: на Linux — флаг `MSG_NOSIGNAL` при `send`.
- IPv4 + IPv6 через `getaddrinfo`.
- Учебная цель: **блокирующий** сервер «один клиент за раз», отвечает `HTTP/1.1 200 OK`.

**Тесты:** RAII закрывает дескриптор, move не приводит к двойному close,
loopback-соединение отправляет/получает байты.

### Фаза 3. Абстракция Poller (≈ 1,5 недели) — сердце проекта

```cpp
enum class Interest : std::uint8_t { None = 0, Read = 1, Write = 2, ReadWrite = 3 };

struct PollEvent {
    native_socket_t fd;
    bool readable, writable, error, hangup;
    void* user_data;
};

class Poller {
public:
    virtual ~Poller() = default;
    virtual void add(native_socket_t fd, Interest, void* user_data) = 0;
    virtual void modify(native_socket_t fd, Interest) = 0;
    virtual void remove(native_socket_t fd) = 0;
    // timeout = время до ближайшего таймера; возвращает число событий в out
    virtual std::size_t wait(std::span<PollEvent> out, std::chrono::milliseconds timeout) = 0;
    static std::unique_ptr<Poller> create();   // выбирает backend по платформе
};
```

- **`EpollPoller`**: `epoll_create1(EPOLL_CLOEXEC)`, `epoll_ctl`, `epoll_wait`.
  Режим **level-triggered** — так семантика совпадает с WSAPoll. (Edge-triggered можно
  добавить позже как оптимизацию и рассказать о разнице на собеседовании.)
- **`WsaPollPoller`**: вектор `WSAPOLLFD` + `unordered_map<SOCKET, index>`
  для O(1) удаления (swap-with-last). Флаги `POLLRDNORM/POLLWRNORM`.
- **Wakeup** (разбудить `wait()` из другого потока): Linux — `eventfd`;
  Windows — пара loopback-сокетов (аналога eventfd для WSAPoll нет).
- Подвох WSAPoll: до Windows 10 2004 не сообщал об ошибке неудачного `connect`.
  Для сервера не критично, но стоит упомянуть в README.

**Тесты (одинаковые для обоих backend-ов, typed tests GoogleTest):** событие чтения
приходит после `send` с другой стороны; Write приходит сразу; после `remove` событий нет;
wakeup будит `wait`; timeout соблюдается.

### Фаза 4. EventLoop, буферы, таймеры (≈ 1 неделя)

- `EventLoop::run()`: `wait` → обработка событий → истёкшие таймеры → задачи из очереди.
- `post(std::function<void()>)` — потокобезопасная очередь задач + wakeup.
- `TimerQueue`: min-heap по времени, `add_timer`, `cancel`. Время до ближайшего таймера
  = timeout для `poller.wait`.
- `Buffer`: непрерывный байтовый буфер с `read_pos/write_pos`, компакция, лимит размера.
- `Connection`: read-буфер, write-буфер, подписка на Write **только** когда есть что писать
  (иначе level-triggered даст 100% CPU). Корректное закрытие: `shutdown` после отправки
  ответа, затем close.
- **Backpressure:** если write-буфер > N КБ — перестать читать от клиента.

**Готово, когда:** однопоточный неблокирующий echo-сервер держит 1000 соединений
(проверка скриптом) на обеих ОС.

### Фаза 5. HTTP/1.1 парсер (≈ 1,5–2 недели)

- **Инкрементальный конечный автомат**: данные приходят кусками, парсер возвращает
  `NeedMore / Complete / Error(status)`. Без копий — `std::string_view` в буфер.
- Стартовая строка, заголовки (регистронезависимые имена), `Content-Length`,
  `Transfer-Encoding: chunked`, `Connection: keep-alive/close`, `Host` обязателен в 1.1.
- **Лимиты** (защита): длинная строка запроса → `414`, большие заголовки → `431`,
  большое тело → `413`, неизвестная версия → `505`, мусор → `400`.
- Одновременно `Content-Length` и `Transfer-Encoding` → `400` (защита от request smuggling).
- Keep-alive и **pipelining**: несколько запросов в одном пакете обрабатываются по порядку.
- `Response` builder: статус, заголовки, `Date`, `Content-Length`, `Server: epollo`.

**Тесты:** таблица корректных/некорректных запросов; запрос, поданный по одному байту,
даёт тот же результат, что целиком; pipelining.
**Фаззинг:** `fuzz/fuzz_http_parser.cpp` под libFuzzer + ASan — прогнать час, найденные
кейсы добавить в unit-тесты. Строчка «фаззер нашёл N багов, все исправлены» в README — очень сильная.

### Фаза 6. Роутинг и статика (≈ 1 неделя)

- `Router`: `GET /path` → handler, параметры пути (`/api/echo/{id}`), `404`, `405` + `Allow`.
- Статические файлы из `www/`:
  - URL-декодирование, нормализация пути, **защита от path traversal**
    (`..`, `%2e%2e`, `\` на Windows, абсолютные пути, симлинки наружу) → `403/404`;
  - MIME по расширению; `index.html` для каталогов;
  - `Last-Modified` / `If-Modified-Since` → `304`; `ETag` (опционально);
  - большие файлы отдавать кусками, не читая целиком в память.
- Поддержка `HEAD`.
- Встроенные эндпоинты: `/health`, `/metrics` (счётчики запросов, активные соединения).

### Фаза 7. Многопоточность (≈ 1,5 недели)

- Модель **«one loop per thread»**: N потоков = N `EventLoop`, у каждого свой `Poller`.
  Соединение всю жизнь живёт в одном потоке → его данные без мьютексов.
- Распределение соединений:
  - Linux: `SO_REUSEPORT` — у каждого потока свой listen-сокет, ядро балансирует;
  - Windows (и fallback): один acceptor-поток, round-robin, передача сокета
    в нужный loop через `post()` + wakeup.
- Корректное завершение: `SIGINT/SIGTERM` (Linux), `SetConsoleCtrlHandler` (Windows) →
  перестать принимать, дождаться активных запросов (с дедлайном), остановить потоки.
- `std::jthread` + `std::stop_token`.

**Тесты:** нагрузочный integration-тест под **ThreadSanitizer** (Linux) — ноль гонок.

### Фаза 8. TLS / HTTPS (≈ 2 недели)

- `TlsContext` (RAII над `SSL_CTX`): загрузка сертификата и ключа, **только TLS 1.2+**,
  современные шифры, ALPN `http/1.1`.
- `TlsSession` (RAII над `SSL` + два memory BIO):
  ```
  сокет → read-буфер → BIO_write(rbio) → SSL_read  → открытый текст → HTTP парсер
  HTTP ответ → SSL_write → BIO_read(wbio) → write-буфер → сокет
  ```
  Обработка `SSL_ERROR_WANT_READ/WANT_WRITE`, неблокирующий handshake,
  корректный `close_notify`.
- `Connection` получает стратегию транспорта: `PlainTransport` / `TlsTransport` —
  HTTP-код не знает, шифруется соединение или нет.
- Два порта: `8443` HTTPS и `8080` HTTP с `301`-редиректом на HTTPS (опция).
- Скрипт генерации self-signed сертификата для разработки; сертификат Let's Encrypt
  подключается просто путями к файлам.
- Таймаут handshake.

**Проверка:** `curl -v --cacert ... https://localhost:8443`, `openssl s_client`,
нет утечек под ASan/Valgrind после тысяч handshake-ов.

### Фаза 9. Надёжность, конфиг, логи (≈ 1 неделя)

- Таймауты: чтение заголовков (защита от **slowloris**), idle keep-alive, запись.
- Лимит одновременных соединений; при превышении — `503` или закрытие.
- Конфигурация: аргументы CLI (`--port`, `--threads`, `--root`, `--cert`, `--key`)
  + простой конфиг-файл `key=value` со своим парсером.
- Логирование: уровни, access log в Common Log Format, без блокировок на горячем пути
  (буфер на поток).
- Dockerfile (Linux-сборка), пример запуска.

### Фаза 10. IOCP backend (≈ 2–3 недели, «звезда» проекта)

- Над reactor-ом вводится completion-интерфейс:
  ```cpp
  class AsyncStream {
  public:
      virtual ~AsyncStream() = default;
      virtual void async_read(std::span<std::byte> buf, ReadHandler) = 0;
      virtual void async_write(std::span<const std::byte> buf, WriteHandler) = 0;
  };
  ```
- Linux: `ReactorStream` — реализация поверх существующего epoll-loop.
- Windows: `IocpStream` — `CreateIoCompletionPort`, `WSARecv/WSASend` с `OVERLAPPED`,
  `AcceptEx` для приёма, `GetQueuedCompletionStatusEx` в цикле, `PostQueuedCompletionStatus`
  для wakeup. Буфер принадлежит операции до её завершения, отмена через `CancelIoEx`.
- Выбор backend-а на Windows флагом `--backend=wsapoll|iocp`.
- **Бенчмарк WSAPoll vs IOCP** — таблица в README. Это и есть ответ на вопрос
  «зачем ты всё это делал».

### Фаза 11. Бенчмарки и упаковка (≈ 3–4 дня)

- `bench/`: скрипты `wrk -t4 -c256 -d30s`, результаты для plain/TLS,
  1/4/8 потоков, epoll / WSAPoll / IOCP. Указать железо.
- Профилирование (`perf` / VS Profiler), одна-две оптимизации с цифрами «до/после».
- README: что это, фичи, схема архитектуры, сборка на обеих ОС,
  результаты бенчмарков, известные ограничения, что можно улучшить.
- Релиз `v1.0` с бинарниками через GitHub Releases.

---

## 4. Вехи

| Веха | Фазы | Что можно показать | ~Срок |
|---|---|---|---|
| M1 | 1–2 | CI на двух ОС, блокирующий HTTP-ответ | 2 нед |
| M2 | 3–4 | Неблокирующий сервер на epoll и WSAPoll | +2,5 нед |
| M3 | 5–6 | Настоящий HTTP/1.1 сервер со статикой, фаззинг | +3 нед |
| M4 | 7 | Многопоточность, TSan чистый | +1,5 нед |
| M5 | 8–9 | **HTTPS**, таймауты, конфиг — уже можно в резюме | +3 нед |
| M6 | 10–11 | IOCP + бенчмарки — проект выше уровня junior | +3 нед |

Ссылку в резюме можно ставить с **M3**, полноценно — с **M5**.

---

## 5. Чего избегать

- `#ifdef _WIN32` по всему коду — только в `platform/`.
- Голые `new/delete`, `malloc` — только RAII и умные указатели.
- Копии `std::string` на каждый заголовок — `string_view` в буфер.
- Подписка на Write без данных для отправки (busy loop).
- Мьютекс на каждое соединение — вместо этого «соединение принадлежит одному потоку».
- Писать всё сразу — сначала работает, потом быстро.
- Гигантские коммиты «сделал всё» — маленькие осмысленные коммиты видны работодателю.

---

## 6. Темы для собеседования, которые даёт проект

- Reactor vs Proactor и почему epoll и IOCP не абстрагируются «в лоб».
- Level- vs edge-triggered epoll.
- Зачем memory BIO для TLS.
- Защита от slowloris, path traversal, request smuggling.
- Что нашёл фаззер.
- Цифры бенчмарков и что ограничивает производительность.
