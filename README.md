# pawprint

A substition of systemd-tmpfiles

## Features

- Small size (less than 1k lines of code),single file
- systemd-free
- POSIX compatible

## Installation

Available on eweOS

```shell
pacman -S pawprint
```

To compile from source,simply run

```shell
cc pawprint.c -o pawprint -DARCH=x86_64  # For x86-64 platform
```

You need to define macro ARCH as your platform,its value could be:
- ``x86_64``: x86-64
- ``aarch64``: arm64

## Special Options (Different from systemd-tmpfiles)

- ``--no-default``: Do not parse the default configuration files
(in ``/lib/tmpfiles.d`` and ``/etc/tmpfiles.d``)
- ``--log``: Specify where to print log.It will be printed to ``stderr``
without ``--log`` option.

## Supported Types

These types are supported in the configuration file

- ``q`` & ``Q``,see Known Issues
- ``w``
- ``f``
- ``d`` & ``D``
- ``h``
- ``x``
- ``!`` (modifier)

## Known Issues

- ``Q`` and ``q`` types won't create a subvolume even possible
- Specifiers are ***NOT*** recognised

## About

``pawprint`` is a part of eweOS project,mainly developed by Ziyao.

By MIT License.

For more information about the configuration file,see systemd-tmpfiles
manual
