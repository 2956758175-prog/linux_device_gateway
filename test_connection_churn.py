
import socket
import time

SERVER_IP = "192.168.92.129"
SERVER_PORT = 8080

TEST_COUNT = 100


def main():
    success_count = 0
    failed_count = 0

    print("========== TCP Connection Churn Test ==========")
    print(f"Target: {SERVER_IP}:{SERVER_PORT}")
    print(f"Total attempts: {TEST_COUNT}")

    for i in range(TEST_COUNT):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3)

        try:
            sock.connect((SERVER_IP, SERVER_PORT))
            success_count += 1

            # 留出时间，让服务端有机会 accept 该连接
            time.sleep(0.01)

            sock.close()

        except OSError as e:
            failed_count += 1
            print(f"Connection {i + 1} failed: {e}")

        finally:
            sock.close()

        if (i + 1) % 10 == 0:
            print(
                f"Progress: {i + 1}/{TEST_COUNT}, "
                f"Success: {success_count}, "
                f"Failed: {failed_count}"
            )

        time.sleep(0.01)

    # 等待服务端处理最后几个断开事件
    time.sleep(2)

    print("\n========== Test Result ==========")
    print(f"Successful connections: {success_count}")
    print(f"Failed connections: {failed_count}")

    if failed_count == 0:
        print("TCP connection churn test PASSED.")
    else:
        print("TCP connection churn test FAILED.")


if __name__ == "__main__":
    main()

