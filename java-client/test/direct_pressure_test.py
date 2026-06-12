import socket
import struct
import time
import threading
import os

# ==================== 长连接峰值压测配置 ====================
TARGET_IP = "172.31.14.229"
TARGET_PORT = 9000
INPUT_IMAGE = "test.png"
RPC_TYPE = 3
printed = False
CONCURRENT_THREADS = 1600

WARMUP_SECONDS = 10
TEST_SECONDS = 30
# ==========================================================

FIELD_TAG = b'\x0A'

success_count = 0
failure_count = 0

peak_tps = 0

stats_lock = threading.Lock()
stop_event = threading.Event()

latencies = []


def encode_varint(value):
    bits = value & 0x7F
    value >>= 7
    ret = bytearray()

    while value:
        ret.append(bits | 0x80)
        bits = value & 0x7F
        value >>= 7

    ret.append(bits)

    return bytes(ret)


def decode_varint(data, pos):
    result = 0
    shift = 0

    while True:
        b = data[pos]
        pos += 1

        result |= (b & 0x7F) << shift

        if not (b & 0x80):
            break

        shift += 7

    return result, pos


def recv_exact(sock, size):
    buf = b""

    while len(buf) < size:
        chunk = sock.recv(size - len(buf))

        if not chunk:
            raise ConnectionError("socket disconnected")

        buf += chunk

    return buf


def do_rpc(sock, full_packet):
    global success_count
    global failure_count

    rpc_start = time.perf_counter()

    try:
        sock.sendall(full_packet)

        header_bytes = recv_exact(sock, 16)

        magic, version, resp_body_len, rtype = struct.unpack(
            "!IIII",
            header_bytes
        )

        resp_body = recv_exact(sock, resp_body_len)
        global printed
        if printed==False:
            print(resp_body[:200])
            print(resp_body_len)
            printed = True
        pos = 0
        is_valid = False

        while pos < len(resp_body):

            tag, pos = decode_varint(resp_body, pos)

            if (tag & 0x07) == 2:

                length, pos = decode_varint(resp_body, pos)

                if len(resp_body[pos:pos + length]) > 100:
                    is_valid = True

                pos += length

            else:
                _, pos = decode_varint(resp_body, pos)

        rpc_end = time.perf_counter()

        latency_ms = (rpc_end - rpc_start) * 1000

        if latency_ms > 50:
            print(latency_ms)

        with stats_lock:

            latencies.append(latency_ms)

            if is_valid:
                success_count += 1
            else:
                failure_count += 1

    except Exception:

        print("RPC ERROR:", repr(e))
        with stats_lock:
            failure_count += 1

        raise


def worker_thread(full_packet):

    while not stop_event.is_set():

        sock = None

        try:

            sock = socket.socket(
                socket.AF_INET,
                socket.SOCK_STREAM
            )

            sock.settimeout(10.0)

            sock.connect(
                (TARGET_IP, TARGET_PORT)
            )

            while not stop_event.is_set():

                do_rpc(
                    sock,
                    full_packet
                )

        except Exception:

            try:
                if sock:
                    sock.close()
            except:
                pass

            time.sleep(0.1)

        finally:

            try:
                if sock:
                    sock.close()
            except:
                pass


def monitor_thread():

    global peak_tps

    last_success = 0

    while not stop_event.is_set():

        time.sleep(1)

        with stats_lock:
            current_success = success_count

        current_tps = current_success - last_success

        peak_tps = max(
            peak_tps,
            current_tps
        )

        print(
            f"[TPS] current={current_tps} "
            f"peak={peak_tps}"
        )

        last_success = current_success


def percentile(data, p):

    if not data:
        return 0

    idx = int(len(data) * p)

    idx = min(idx, len(data) - 1)

    return data[idx]


def main():

    global success_count
    global failure_count
    global peak_tps

    if not os.path.exists(INPUT_IMAGE):

        print(
            f"Error: 找不到图片 {INPUT_IMAGE}"
        )

        return

    print("正在预组装请求报文...")

    with open(INPUT_IMAGE, "rb") as f:
        img_bytes = f.read()

    varint_len = encode_varint(
        len(img_bytes)
    )

    body_bytes = (
        FIELD_TAG +
        varint_len +
        img_bytes
    )

    header = struct.pack(
        "!IIII",
        0xCAFEBABE,
        1,
        len(body_bytes),
        RPC_TYPE
    )

    full_packet = header + body_bytes

    print("\n========== 长连接峰值压测 ==========")
    print(f"目标节点      : {TARGET_IP}:{TARGET_PORT}")
    print(f"线程数        : {CONCURRENT_THREADS}")
    print(f"预热时间      : {WARMUP_SECONDS}s")
    print(f"测试时间      : {TEST_SECONDS}s")
    print("===================================")

    workers = []

    for _ in range(CONCURRENT_THREADS):

        t = threading.Thread(
            target=worker_thread,
            args=(full_packet,)
        )

        t.start()

        workers.append(t)

    monitor = threading.Thread(
        target=monitor_thread
    )

    monitor.start()

    print("\n开始预热...")
    time.sleep(WARMUP_SECONDS)

    with stats_lock:

        success_count = 0
        failure_count = 0

        latencies.clear()

    peak_tps = 0

    print("\n开始正式测试...")

    start_time = time.time()

    time.sleep(TEST_SECONDS)

    stop_event.set()

    for t in workers:
        t.join()

    monitor.join()

    end_time = time.time()

    actual_duration = TEST_SECONDS

    total_processed = (
        success_count +
        failure_count
    )

    accuracy = (
        success_count /
        total_processed *
        100
        if total_processed > 0
        else 0
    )

    qps = (
        total_processed /
        actual_duration
    )

    latencies.sort()

    avg_latency = (
        sum(latencies) / len(latencies)
        if latencies else 0
    )

    p95 = percentile(
        latencies,
        0.95
    )

    p99 = percentile(
        latencies,
        0.99
    )

    print("\n============== 测试报告 ==============")

    print(f"线程数          : {CONCURRENT_THREADS}")
    print(f"测试时长        : {TEST_SECONDS}s")

    print(f"成功请求        : {success_count}")
    print(f"失败请求        : {failure_count}")

    print(f"平均TPS         : {qps:.2f}")
    print(f"峰值TPS         : {peak_tps}")

    print(f"平均延迟(ms)    : {avg_latency:.2f}")
    print(f"P95延迟(ms)     : {p95:.2f}")
    print(f"P99延迟(ms)     : {p99:.2f}")

    print(f"准确率          : {accuracy:.2f}%")

    print("======================================")


if __name__ == "__main__":
    main()