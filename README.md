# pawprint

A substition of systemd-tmpfiles

## Features

- Small size (less than 1k lines of code), single file
- systemd-free
- POSIX compatible

## Installation

Available on eweOS:

```shell
pacman -S pawprint
```

To compile from source, simply run:

```shell
cc pawprint.c -o pawprint
```

## Special Options (different from systemd-tmpfiles)

- `--no-default`: Do not parse the default configuration files
(in `/lib/tmpfiles.d` and `/etc/tmpfiles.d`)
- `--log`: Specify where to print log.It will be printed to `stderr`
without `--log` option.

## Supported Types

These types are supported in the configuration file:

- `q` & `Q` (see [Known Issues](#known-issues))
- `w`
- `f`
- `d` & `D`
- `h`
- `x`
- `!` (modifier)

## Known Issues

- `Q` and `q` types won't create a subvolume even if possible
- Modifiers other than '!' are not recognised
- Will follow symlink even if `tmpfiles.d(5)` says otherwise

## About

`pawprint` is a part of eweOS project, mainly developed by Ziyao, and
is licensed under the MIT License.

For more information about the configuration file, see
systemd-tmpfiles' manual.
