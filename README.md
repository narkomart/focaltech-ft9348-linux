# Отпечаток FocalTech FT9348 под Linux

> 🇷🇺 Проект из России. Автор: **[narkomart](https://github.com/narkomart)**.
> Русскоязычный проект по запуску сканера отпечатков FocalTech под Linux.
> [English version below ↓](#focaltech-ft9348-fingerprint-under-linux-english)

Заставляем сканер отпечатков **FocalTech FT9348** (USB `2808:9348`, внутренний чип `0x9362`,
матрица 64×80, обычно встроен в **кнопку питания** ноутбука) работать под Linux — где для него
**нет рабочего драйвера**.

**Статус:** захват, регистрация и сравнение отпечатков работают; вход по отпечатку подключён через PAM.
Проверено: вход по отпечатку в **sudo** и **разблокировка экрана GNOME** отпечатком. После регистрации
по 30 касаниям — 9 входов в sudo из 9, обычно с первого-второго касания; чужой палец не прошёл ни разу.

> Всё началось с того, что штатные `libfprint`/`fprintd` содержат только драйвер Goodix, community
> kernel-драйверы написаны под другие ревизии FocalTech (`9338`/`93a9`/`9e65`, не `9348`), а чип
> говорит по протоколу **регистры-через-control-transfer**, под который существующий bulk-код никогда
> не подходил.

## ⚠️ Важно: репозиторий не самодостаточен

Для сравнения используется **проприетарный** `.so` FocalTech (libfprint) с CNN-матчером (GhostNet).
Этот бинарник **защищён авторским правом и НЕ входит в репозиторий.** Его нужно раздобыть самому и
положить сюда:

```
/opt/focaltech/matcher.so
```

**Где взять** (скачайте `.deb`/`.rpm`/`.so` сами; мы его не распространяем):
- [github.com/ftfpteams](https://github.com/ftfpteams) — официальные проприетарные драйверы libfprint от FocalTech.
- [github.com/leopalladium/focaltech-ft9366-arch-shim](https://github.com/leopalladium/focaltech-ft9366-arch-shim) —
  пропатченный проприетарный драйвер + шим для современных дистрибутивов.

Дальше хелпер сам извлечёт, **проверит и установит** его:

```sh
sudo bash bio/get-matcher.sh /path/to/driver.deb     # или .rpm, или сам .so, или каталог
sudo bash bio/get-matcher.sh --search                # или просканировать систему на совместимый .so
```

Он проверяет, что файл — совместимая сборка FocalTech (есть нужные символы), прежде чем установить в
`/opt/focaltech/matcher.so`, поэтому неверный файл падает громко, а не молча.

**Никаких ручных offset-ов.** `ftbio` находит нужные функции и глобалы `focal_*` **по имени** из
таблицы символов `.so` в рантайме — подойдёт любая совместимая (не stripped) сборка FocalTech, адреса
править не нужно. (Исторический факт: самым тяжёлым багом в проекте было чтение offset-ов из Ghidra,
которая грузит образ с базой `+0x100000`, вместо `nm`. Резолв по имени в рантайме снимает эту проблему
полностью.) Проверка совместимости вашего `.so`: `sudo /opt/focaltech/ftbio check`.

## Как это устроено

Захват изображения — наш собственный код поверх libusb по регистровому control-transfer протоколу чипа.
Сравнение — проприетарный CNN FocalTech: загружаем `matcher.so` (`dlopen`, `RTLD_LAZY`) и вызываем его
неэкспортируемые функции `focal_*`, найденные по имени в таблице символов `.so`. Всё работает в одном
процессе, так что конфликта с системным `libfprint` нет. Вход идёт через `pam_exec` — без `fprintd`.

Полный протокол и API матчера: [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## История: как мы это сделали

Краткая хроника — от «драйвера нет» до входа по пальцу. Ноутбук `Pro-Magnus-M-DN16R7`, сенсор в
кнопке питания.

0. **Проблема.** `fprintd`/`libfprint` стоят и исправны, но знают только Goodix → `fprintd-enroll` =
   «No devices available». Сканер физически есть, а для системы его как бы нет.
1. **Community-драйвер — тупик.** Взяли `ft9201`-драйвер, добавили PID `0x9348`: модуль собрался,
   `probe()` сработал, но протокол не совпал — все регистры читались константой `0x0a0a`, кадр = 0 байт.
   Community-код написан под `9338/93a9/9e65`, а не под наш чип.
2. **Разбор Windows-драйвера (Ghidra).** Декомпилировали официальный `ftWbioUmdfDriverV2.dll`: это
   семейство **FT9366 (9368/9369)**, user-mode поверх WinUSB. Выяснили ключевое: детект пальца =
   чтение регистра `0x43`, а регистры читаются/пишутся **не bulk-командами, а USB control-transfer**.
   Именно поэтому Linux-путь (`ft9366.c`) с bulk `02 00 …` вечно таймаутил.
3. **Проприетарный libfprint FocalTech — частичный успех.** Поставили закрытый `.so` + шим + патч
   USB-ID: `fprintd` **увидел** сенсор, open/init прошли, прочитался chip id `0x9362`, загрузился
   алгоритм. Но захват не пошёл — библиотека для нашего чипа шла тем же неверным bulk-путём. Зато
   подтвердилось: внутри `.so` есть **рабочий матчер FocalTech**.
4. **Прорыв по протоколу (на железе).** Реализовали регистры через control-transfer: чтение = control
   IN `bmRequestType=0xC0, bRequest=<reg>`. **Reg `0x43`: `0x00` без пальца / `0x01` с пальцем —
   заработало.** Control по EP0 работает даже когда bulk «завис».
5. **Захват изображения.** Реверснули последовательность съёмки кадра: wake → sensor mode →
   `0x34=0xff`; `0x34=3`; `0x6f(wIndex=0x9080, wValue=5120)`; **bulk-IN 5120 байт**. Получили настоящее
   изображение **64×80 с гребнями отпечатка**.
6. **Своя попытка на NBIS — потолок.** Написали libfprint-драйвер: захват + свайп-сборка → минуции →
   enroll завершается. Но verify давал score 0: generic-матчер NBIS ненадёжен на таком микро-сенсоре.
   Вывод: нужен **родной матчер FocalTech**.
7. **Интеграция матчера — главный прорыв.** Матчер оказался **свёрточной сетью (GhostNet)** с
   анти-спуфом. Харнесс падал в `ff_algo_do_config` из-за **опечатки в один hex-символ**: offset брали
   из Ghidra (`0x143c20`), а она грузит образ с базой `+0x100000`; реальный адрес из `nm` — `0x43c20`.
   После фикса: конфиг = 0, извлечение template из кадра, сравнение двух шаблонов работает. **Правило:
   offset-ы — только из `nm`, не из Ghidra.**
8. **Проверка точности (на реальных пальцах).** 12 кадров своего пальца → все совпали (TAR 100%);
   8 кадров чужого → все отвергнуты (FAR 0%). Матчер FocalTech надёжно различает пальцы.
   *Позже выяснилось, что «100%» — артефакт:* проверяли на тех же кадрах, по которым регистрировали.
9. **Продукт: автономный бинарь + вход.** Собрали `bio/ftbio` (наш захват + матчер по имени символа).
   Вход по пальцу в **sudo** (PAM) и **разблокировка экрана GNOME** — работают.
10. **«Узнаёт через раз» (ноябрь 2026).** В работе палец узнавался примерно через раз. Честный замер на
   кадрах, которых нет в шаблоне: шаблон из 6 касаний узнаёт 1 новое касание из 6, из 11 касаний —
   9 из 12; чужой палец — 0 во всех вариантах. Сенсор 3×4 мм, и 12 касаний покрывают палец лишь частично.
   Второе: после совпадения матчер сообщает `upd=1`, но сам шаблон не меняет — нужен
   `focal_UpdateTemplate` и сохранение, причём только при `actMode=2` в `focal_IdentifyByImage`
   (как в драйвере FocalTech; с `1` обновление отказывает). Третье: gdm мог запустить проверку дважды,
   две копии дрались за USB, и сенсор переподключался. Исправлено: регистрация по 30 касаниям,
   самообучение шаблона после каждого входа (матчер сам ограничивает рост ~0,75 МБ), одна проверка
   за раз (`flock`), строка в журнале на каждую попытку. Итог — 9 из 9 на железе.

Итог: два наложившихся заблуждения — неверный транспорт (bulk вместо control) и неверный матчер (NBIS
вместо родного CNN). Архитектура решения: **наш захват (libusb/control) + проприетарный CNN-матчер
(по символу из `.so`)** в одном процессе, вход — через `pam_exec`, без `fprintd`.

## Структура

```
tools/     инструменты libusb: ftctl (чтение регистра), ftgrab2 (захват кадра), ftenroll (снять N кадров пальца)
matcher/   исследовательские харнессы: verify (сравнение шаблонов), enroll_test (enroll/save/load/identify)
bio/       ftbio.c — автономный enroll/verify (захват + матчер); скрипты установки PAM
driver/    focaltech9348.c — image-драйвер libfprint (захват + свайп-сборка + NBIS). NBIS на этом крошечном
           сенсоре ненадёжен, оставлен для справки. Надёжный путь — bio/ftbio + CNN-матчер.
docs/      PROTOCOL.md — реверснутый протокол и заметки по матчеру
```

## Сборка и использование

```sh
# 0) поставить проприетарный .so FocalTech как матчер (с проверкой):
sudo bash bio/get-matcher.sh /path/to/driver.deb   # или .rpm / .so / каталог, или: --search

# 1) собрать автономный инструмент
cd bio && bash build.sh          # -> ./ftbio   (нужны dev-заголовки libusb-1.0)

# 2) освободить устройство (чтобы fprintd его не держал)
sudo systemctl stop fprintd

# 3) зарегистрировать палец: 30 касаний сенсора в кнопке питания, каждый раз сдвигая и поворачивая
#    палец, захватывая и края, и подушечку (от этого зависит, как часто палец будет узнаваться)
sudo ./ftbio enroll "$USER"      # шаблон -> /etc/focaltech/<user>.tpl

# 4) проверить (одно касание)
sudo ./ftbio verify "$USER"      # печатает «совпадение» / exit 0 при совпадении
```

### Отпечаток для sudo
```sh
sudo bash bio/install-pam-sudo.sh      # добавляет отпечаток как `sufficient`; пароль остаётся резервом
# проверить в НОВОМ терминале:
sudo -k; sudo id                        # коснуться сенсора
```

### Отпечаток для экрана блокировки / входа GNOME
```sh
sudo bash bio/setup-gdm.sh
# заблокировать Super+L и разблокировать касанием сенсора (нет касания ~6с -> поле пароля)
```
Разблокировка экрана работает надёжно. Полноценный вход на greeter GDM может не успеть выполнить
`pam_exec` до отправки пароля (особенность GNOME); для нативной интеграции в greeter/настройки
правильный путь — отдать CNN-матчер самому libfprint-драйверу вместо NBIS.

Переменные окружения `ftbio`: `FT_SO` (путь к матчеру `.so`), `FT_TPLDIR` (каталог шаблонов, по
умолчанию `/etc/focaltech`), `FT_TIMEOUT` (таймаут verify, мс), `FT_ENROLL_N` (касаний при регистрации,
по умолчанию 30), `FT_NO_UPDATE=1` (не дообучать шаблон), `FTLOG=1` (подробный лог алгоритма).

Каждая проверка пишет строку в журнал — `journalctl -t ftbio`: совпало ли, сколько было касаний и
сравнённых кадров, оценка совпадения, обновился ли шаблон. Если палец узнаётся плохо — перерегистрируйте
его (`sudo ./ftbio enroll "$USER"`): шаблон, снятый по 12 касаниям старой версией, слишком мал.

## Восстановление доступа

Отпечаток настроен как `sufficient`, поэтому пароль всегда остаётся резервом — заблокировать себя
нельзя. Правки PAM бэкапятся (`/etc/pam.d/<svc>.bak.<ts>`). Если GUI-вход глючит — переключитесь на TTY
(Ctrl+Alt+F3), войдите по паролю и восстановите бэкап.

## Безопасность / приватность

Не коммитьте биометрию (`*.tpl`, `*.raw`, `cap*/`) и проприетарный `.so` — см. `.gitignore`. Шаблоны
отпечатков — персональные данные; держите `/etc/focaltech` доступным только root (инструмент ставит `0600`).

## Лицензия

GPLv2-or-later (см. [`LICENSE`](LICENSE)). `driver/focaltech9348.c` производен от libfprint и совместим
с GPL/LGPL. Проект не включает и не перелицензирует код FocalTech.

## Авторство

Реверс-инжиниринг и интеграция для FT9348 — **[narkomart](https://github.com/narkomart)**, Россия. 🇷🇺
Проприетарный матчер и Windows/Linux-драйверы FocalTech — © FocalTech; проект взаимодействует с
пользовательской копией и не распространяет её.

---

# FocalTech FT9348 fingerprint under Linux (English)

> 🇷🇺 A project from Russia by **[narkomart](https://github.com/narkomart)**.

Making the **FocalTech FT9348** fingerprint sensor (USB `2808:9348`, internal chip `0x9362`, 64×80
sensor, commonly built into the laptop **power button**) work under Linux — where it has **no working
driver**.

**Status:** capture, enrollment and matching work; fingerprint login is wired via PAM. Verified: **sudo**
login by fingerprint and **GNOME screen unlock** by fingerprint. After a 30-touch enrollment: 9 of 9 sudo
logins, usually on the first or second touch; a different finger never matched. The template self-learns
after every successful login (`focal_UpdateTemplate`, needs `actMode=2`); each attempt is logged —
`journalctl -t ftbio`.

**Not self-contained:** matching uses FocalTech's **proprietary** libfprint `.so` (a GhostNet CNN
matcher), which is copyrighted and **NOT included**. Supply it yourself and let the helper validate and
install it to `/opt/focaltech/matcher.so`:

```sh
sudo bash bio/get-matcher.sh /path/to/driver.deb   # or .rpm / .so / dir, or: --search
```

Get it from [github.com/ftfpteams](https://github.com/ftfpteams) or
[leopalladium/focaltech-ft9366-arch-shim](https://github.com/leopalladium/focaltech-ft9366-arch-shim).
`ftbio` resolves the `focal_*` functions **by name** from the `.so` symbol table at runtime — no manual
offsets, any compatible non-stripped build works.

**How it works:** our own libusb capture over the chip's control-transfer register protocol + FocalTech's
proprietary CNN matcher (`dlopen`, symbols by name), all in one process, login via `pam_exec` (no
`fprintd`). Full protocol and matcher API: [`docs/PROTOCOL.md`](docs/PROTOCOL.md). Build & usage,
recovery and the full story of how it was reverse-engineered are in the Russian section above.

**License:** GPLv2-or-later. Reverse engineering and integration by
**[narkomart](https://github.com/narkomart)** (Russia). The proprietary matcher and FocalTech drivers are
© FocalTech; this project interoperates with a user-supplied copy and does not redistribute it.
