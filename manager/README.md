# MTL Manager Documentation

## Overview

![design](manager-design.svg)

MTL Manager is a daemon server designed to operate with root privileges. Its primary role is to manage the lifecycle and configurations of MTL instances. It addresses a variety of administrative tasks, including:

- Lcore Management: Ensures that MTL instances are aware of the lcores used by others, optimizing resource allocation.
- eBPF/XDP Loader: Dynamically loads and manages eBPF/XDP programs for advanced packet processing and performance tuning.
- NIC Configuration: Configures Network Interface Cards (NICs) with capabilities like adding or deleting flow and queue settings.
- Instances Monitor: Continuously monitors MTL instances, providing status reporting and clearing mechanisms.

## Build

To compile the MTL Manager, use the following commands:

```bash
meson setup build
meson compile -C build
```

## Run

To run the MTL Manager, execute:

```bash
sudo ./build/MtlManager
```

This command will start the MTL Manager with root privileges, which are necessary for the advanced eBPF and network configurations and management tasks it performs.

## Run with our XDP program

We have an edited version of the original AF_XDP eBPF program which allows user to add or remove udp dest port in the eBPF program to act as a packet filter, please see [ebpf tool](../tools/ebpf) for how to build it.

To run the MTL Manager with our XDP program, execute:

```bash
sudo MTL_XDP_PROG_PATH=/path/to/Media-Transport-Library/tools/ebpf/xsk.xdp.o ./build/MtlManager
```