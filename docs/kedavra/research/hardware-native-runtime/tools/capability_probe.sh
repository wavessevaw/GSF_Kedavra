#!/bin/sh
# Только чтение. Не захватывает DRM master, не делает modeset и не пишет sysfs.
# Запуск: sh capability_probe.sh > capability-probe.txt
set -u
printf '# Hardware Native Runtime probe\n'
date -u
uname -a
cat /etc/os-release
command -v pacman >/dev/null 2>&1 && pacman -Q mesa vulkan-radeon gamescope linux-neptune-611 linux-neptune-65 2>/dev/null
command -v vulkaninfo >/dev/null 2>&1 && vulkaninfo 2>&1
for card in /sys/class/drm/card[0-9]*; do
  [ -d "$card/device" ] || continue
  printf '\n## %s\n' "$card"
  readlink -f "$card/device/driver"
  for name in vendor device revision gpu_busy_percent mem_info_vram_total mem_info_gtt_total pp_dpm_sclk pp_dpm_mclk; do
    [ ! -r "$card/device/$name" ] || { printf '%s: ' "$name"; cat "$card/device/$name"; }
  done
  for hw in "$card"/device/hwmon/hwmon*; do
    for item in name power1_label power1_average power1_input freq1_input temp1_input temp1_crit; do
      [ ! -r "$hw/$item" ] || { printf '%s/%s: ' "$hw" "$item"; cat "$hw/$item"; }
    done
  done
done
for con in /sys/class/drm/card*-eDP-*; do
  [ -d "$con" ] || continue
  printf '\n## %s\n' "$con"
  for item in status enabled modes; do
    [ ! -r "$con/$item" ] || cat "$con/$item"
  done
done
for root in /sys/kernel/tracing /sys/kernel/debug/tracing; do
  [ ! -r "$root/available_events" ] || cat "$root/available_events"
done
command -v drm_info >/dev/null 2>&1 && drm_info 2>&1
# Отсутствие доступа фиксировать как unavailable. Не добавлять sudo автоматически.
exit 0
