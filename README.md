# SimpleFS

Учебная файловая система для Linux 6.12.x в виде модуля ядра.

Файловая система работает поверх существующего блочного устройства, использует две копии superblock, отображает заранее созданные файлы в userspace, поддерживает чтение и запись, а также набор ioctl-команд для управления метаданными.

## Требования

- Linux kernel 6.12.x
- заголовки ядра для текущего ядра
- `make`, `gcc`
- `python3`
- root-доступ для `insmod`, `mount`, `ioctl`

## Структура проекта

```text
.
├── Makefile
├── README.md
├── disk.c
├── file.c
├── inode.c
├── ioctl.c
├── main.c
├── mount_simplefs.sh
├── simplefs.h
├── super.c
└── userspace
    ├── ioctl_tool.py
    └── test.py

```

## Сборка

```bash
make
```

После сборки появится модуль:

```bash
simplefs.ko
```

## Параметры модуля

Параметры передаются при загрузке модуля через `insmod`.

- `disk_name` — имя блочного устройства, на базе которого создаётся FS
- `sb1_sector` — сектор первой копии superblock
- `sb2_sector` — сектор второй копии superblock
- `max_filename_len` — максимальная длина имени файла
- `max_file_sectors` — максимальный размер файла в секторах

### Пример загрузки

```bash
sudo insmod simplefs.ko \
  disk_name=/dev/loop15 \
  sb1_sector=1 \
  sb2_sector=8 \
  max_filename_len=32 \
  max_file_sectors=4
```

## Подготовка блочного устройства

Ниже пример с loop-диском и образом `disk.img`.

### Создать образ

```bash
dd if=/dev/zero of=disk.img bs=1M count=20
```

### Привязать к loop-устройству

```bash
sudo losetup -fP --show disk.img
```

Команда выведет, например:

```text
/dev/loop15
```

## Монтирование

Создать точку монтирования:

```bash
sudo mkdir -p /mnt/simplefs
```

Смонтировать FS:

```bash
sudo mount -t simplefs /dev/loop15 /mnt/simplefs
```

Проверить содержимое:

```bash
ls -la /mnt/simplefs
```

## Работа с файлами

Файлы создаются автоматически при инициализации FS.

Примеры:

```bash
echo hello | sudo tee /mnt/simplefs/file00001
cat /mnt/simplefs/file00001
```

```bash
dd if=/dev/urandom of=/mnt/simplefs/file00002 bs=100 count=1
hexdump -C /mnt/simplefs/file00002 | head
```

## IOCTL

Поддерживаются команды:

- `CLEAR` — обнулить все файлы
- `ERASE` — стереть FS
- `GET_HASHES` — получить хэши всех файлов
- `GET_MAP` — получить маппинг секторов для файла

## Userspace CLI

Скрипт `userspace/ioctl_tool.py` вызывает ioctl-команды.

### Очистить все файлы

```bash
sudo python3 userspace/ioctl_tool.py --target /mnt/simplefs clear
```

### Стереть FS

```bash
sudo python3 userspace/ioctl_tool.py --target /mnt/simplefs erase
```

### Получить хэши

```bash
sudo python3 userspace/ioctl_tool.py --target /mnt/simplefs hashes --cap 20000
```

### Получить маппинг для файла

```bash
sudo python3 userspace/ioctl_tool.py --target /mnt/simplefs map file00001
```

## Userspace тест

Скрипт `userspace/test.py` проходит по всем файлам в FS, пишет случайные значения и читает их обратно.

Запуск:

```bash
sudo python3 userspace/test.py
```

## Размонтирование

```bash
sudo umount /mnt/simplefs
```

