import paho.mqtt.client as mqtt
import time
import json
from collections import deque
from statistics import mean

# 設定
BROKER = "localhost"
PORT = 1883
TOPIC = "esp32/test"
WINDOW_SECONDS = 5  # 每 5 秒統計一次 delivery rate 和 latency

# 暫存訊息時間
latency_records = deque()

def on_connect(client, userdata, flags, rc):
    print("Connected with result code", rc)
    client.subscribe(TOPIC)

def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        sent_time = payload.get("timestamp")  # payload 中需有 timestamp 欄位
        print(sent_time)
        recv_time = time.time() * 1000
        print(recv_time)
        latency = recv_time - sent_time  # 將 recv_time 秒轉成毫秒，再減 sent_time（毫秒）
        latency_records.append((recv_time, latency))
    except Exception as e:
        print("Failed to parse message:", e)

def monitor():
    while True:
        now = time.time()
        # 移除太舊的紀錄
        while latency_records and now - latency_records[0][0] > WINDOW_SECONDS:
            latency_records.popleft()

        # 統計
        latencies = [lat for _, lat in latency_records]
        delivery_rate = len(latencies) / WINDOW_SECONDS
        avg_latency = mean(latencies) if latencies else 0

        print(f"[{time.strftime('%H:%M:%S')}] Delivery Rate: {delivery_rate:.2f} msg/sec, Avg Latency: {avg_latency*1000:.2f} ms")
        time.sleep(1)

if __name__ == "__main__":
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER, PORT, 60)

    # 啟動監控線程
    import threading
    monitor_thread = threading.Thread(target=monitor, daemon=True)
    monitor_thread.start()

    # 主迴圈
    client.loop_forever()
