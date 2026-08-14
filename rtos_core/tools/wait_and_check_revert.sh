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

echo "waiting additional 90s for mining bring-up..."
sleep 90

ssh -o StrictHostKeyChecking=no root@$DEVICE_IP 'echo === ps ===; ps | grep -E "mujina|rtos"; echo === ipc6.log tail ===; tail -n 20 /sharefs/ipc6.log; echo === mujina-minerd log tail ===; tail -n 40 /data/mujina-minerd.log 2>/dev/null'
