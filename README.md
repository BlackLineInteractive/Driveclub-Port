# Driveclub Native

Нативний порт **Driveclub** (PS4, CUSA00003 v1.05) на macOS з рендерингом через **Metal 3**.

Це не емулятор загального призначення. Код PS4 — x86‑64, тому на Intel Mac він виконується
напряму, без перекомпіляції CPU. Ми замінюємо лише те, що є специфічним для консолі:
системні бібліотеки Sony (HLE) і GPU AMD Liverpool (GCN) — власним рендерером на Metal,
заточеним під одну гру.

## Архітектура

```
eboot.bin (x86-64, виконується нативно)
   │  імпорти 922 функцій з 57 бібліотек
   ▼
HLE бібліотек Sony ── libkernel, libc, Pad, AudioOut, VideoOut, мережа/PSN → заглушки
   │  sceGnmSubmitCommandBuffers / sceGnmDingDong
   ▼
GPU frontend ── розбір PM4-пакетів, стан регістрів, GCN-шейдери → IR
   │  src/gpu/renderer.h  (спільний інтерфейс)
   ├──► Metal 3 (macOS)  — src/gpu/metal, IR → MSL напряму
   └──► Vulkan (Windows/Linux) — можна підключити рендерер shadPS4
```

Gnm/Gnmx вшиті в сам eboot: гра пише PM4-пакети в буфер команд сама, а драйверу
віддає лише готовий буфер (`sceGnmDraw*` не імпортуються). Тому GPU‑шар працює на рівні
PM4 + GCN‑шейдерів.

### Принципи продуктивності

- IR шейдерів → MSL напряму, без SPIR‑V/SPIRV‑Cross; кеш скомпільованих конвеєрів на диску.
- Argument buffers Tier 2 (bindless) — мінімум викликів прив'язки ресурсів на draw.
- До 3 кадрів у польоті; CPU чекає GPU лише коли випереджає на 3 кадри.
- На дискретних AMD пам'ять не спільна: відстеження змінених сторінок і завантаження лише змін.
- Явний вибір дискретної GPU на MacBook з двома відеокартами (`DC_GPU=<частина назви>` для перевизначення).

## Вимоги

- Intel Mac (x86_64), GPU з підтримкою Metal 3 (перевірено: AMD Radeon Pro 5500M)
- Xcode, CMake ≥ 3.24, Ninja, SDL3 (`brew install cmake ninja sdl3`)
- Власна копія гри у вигляді розшифрованого дампу (fSELF/fPKG)

## Збірка

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/driveclub
```

## Файли гри

Файли гри **не входять** у репозиторій і ніколи не мають туди потрапити (див. `.gitignore`).
Розпакування PKG:

```bash
tools/extract.sh <шлях до .pkg> extracted
```

Аналіз eboot (імпорти за бібліотеками):

```bash
tools/elfinfo.py extracted/Image0/eboot.bin
tools/elfinfo.py extracted/Image0/eboot.bin --lib libSceGnmDriver
```

## Подяки

- [shadPS4](https://github.com/shadps4-emu/shadPS4) — емулятор PS4, головне джерело знань про
  завантаження ELF, HLE бібліотек, PM4 і перекомпіляцію GCN‑шейдерів. Його рендерер на Vulkan
  може слугувати бекендом для Windows/Linux. Зверніть увагу: shadPS4 поширюється під GPL‑2.0.
- [OpenGNM](https://ps4-opengnm.github.io/opengnm/) — документація з Gnm і GCN‑шейдерів PS4.
- Driveclub © Sony Interactive Entertainment / Evolution Studios. Цей проєкт не пов'язаний із Sony.
