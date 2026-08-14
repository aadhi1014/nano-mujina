#!/bin/bash
# Set DEVICE_IP to your Nano3s device's LAN address before running.
DEVICE_IP="${DEVICE_IP:-192.168.1.152}"
set -x
for i in $(seq 1 30); do
    sleep 10
    if ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no root@$DEVICE_IP "echo up" 2>/dev/null | grep -q up; then
        echo "SSH back up after ~$((i*10))s"
        break
    fi
done

echo "waiting additional 100s for full chain bring-up..."
sleep 100

ssh -o StrictHostKeyChecking=no root@$DEVICE_IP 'echo === date/uptime ===; date; cat /proc/uptime; echo === ps ===; ps | grep -E "mujina|rtos"; echo === ipc6.log TAIL (error counters) ===; tail -n 30 /sharefs/ipc6.log; echo === mujina-minerd log tail ===; tail -n 30 /data/mujina-minerd.log 2>/dev/null'
