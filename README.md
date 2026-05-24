# faylu-epshteyna

> Скачивалка видосиков с очень клутых ситов — на зашифрованный/съёмный диск.

Написана на C (V5.2). Нативные экстракторы для популярных сайтов, встроенный HLS-загрузчик, fallback на yt-dlp, дедупликация по partial MD5, атомарная резервация номеров файлов.

---

## Возможности

- **Нативные экстракторы** — без внешних зависимостей для основных сайтов
- **Встроенный HLS** — качает `.ts`-потоки без ffmpeg
- **Fallback на yt-dlp** — если нативный экстрактор не справился
- **Дедупликация** — MD5 по первым и последним 10 МБ файла, не даёт скачать одно дважды
- **Атомарная нумерация** — `fcntl` locks (Linux) / `CreateFile` (Windows), безопасно при параллельных запусках
- **Автоочистка мусора** — удаляет файлы меньше `TRASH_SIZE_MB` по всему диску
- **Кросс-платформа** — Linux и Windows (статический `.exe`, без DLL)
- **Прокси** — хардкод, системный или отключить

---

## Поддерживаемые сайты

| Сайт | Экстрактор |
|---|---|
| pornhub.com | нативный |
| vk.com, ukdevilz.com | нативный |
| xvideos.com | нативный |
| xhamster.com | нативный |
| xnxx.com | нативный |
| eporner.com | нативный |
| redtube.com | нативный |
| sex-studentki.com | нативный |
| ebalko.com | нативный |
| bunkr.si | нативный |
| tiktok.com | нативный |
| thothub.to | нативный |
| yandex (видео) | мета-экстрактор |
| всё остальное | yt-dlp fallback |

---

## Установка и сборка

**Зависимости:** `libcurl`, `openssl`, `cmake`

```bash
# Linux
cmake -B build && cmake --build build

# Windows (кросс-компиляция через MinGW-w64)
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
cmake --build build-win
# → build-win/faylu-epshteyna.exe  (статический, DLL не нужны)
```

---

## Использование

```
faylu-epshteyna [ОПЦИИ]
```

| Флаг | Короткая форма | Описание |
|---|---|---|
| `--url <URL>` | `-u` | URL видео (иначе спросит в stdin) |
| `--output <DIR>` | `-o` | Папка для сохранения (по умолчанию — `SECRET_PATH`) |
| `--quality <N>` | `-q` | Ограничить качество (напр. `720`) |
| `--no-proxy` | `-n` | Прямое подключение |
| `--system-proxy` | `-s` | Системный прокси (`HTTP_PROXY`/`HTTPS_PROXY`) |
| `--user-agent <UA>` | `-A` | Кастомный User-Agent |

**Примеры:**

```bash
# Скачать с URL, качество до 1080p
./faylu-epshteyna --url https://example.com/video --quality 1080

# Без прокси, в кастомную папку
./faylu-epshteyna -u https://example.com/video -n -o /tmp/videos
```

---

## Структура файлов на диске

```
/run/media/xomel45/SECRET/
├── pornhub.com/
│   ├── uchebka_0.mp4
│   ├── uchebka_1.ts        ← HLS-видео
│   └── 3/                  ← временная папка (прерванная загрузка)
├── vk.com/
│   └── uchebka_0.mp4
└── ...
```

---

## Деплой

```bash
./deploy.sh beta --build              # ELF → builds/beta/
./deploy.sh release linux             # .tar.gz → builds/releases/5.x-linux/
./deploy.sh release win               # .exe+zip → builds/releases/5.x-windows/
./deploy.sh release pkg               # .pkg.tar.zst (Arch Linux)
./deploy.sh release deb               # .deb (Debian / Ubuntu)
./deploy.sh release rpm               # .rpm (Fedora / RHEL)
./deploy.sh release all --build       # все форматы разом
```

---

## Лицензия

[Mozilla Public License 2.0](LICENSE)
