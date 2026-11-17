
import socket
import struct
import time

SERVER_IP = "192.168.92.129"
SERVER_PORT = 8080

DEVICE_ID = 3005

FRAME_HEADER = 0xAA55
PROTOCOL_VERSION = 0x01
CMD_REGISTER = 0x01
CMD_RESPONSE = 0x80


def crc16_ccitt(data):
    crc = 0xFFFF

    for byte in data:
        crc ^= byte << 8

        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF

    return crc


def make_frame(command, device_id, payload=b""):
    header = struct.pack(
        "!HBBII",
        FRAME_HEADER,
        PROTOCOL_VERSION,
        command,
        device_id,
        len(payload)
    )

    frame = header + payload
    crc = crc16_ccitt(frame)

    return frame + struct.pack("!H", crc)


def recv_exact(sock, length):
    data = b""

    while len(data) < length:
        chunk = sock.recv(length - len(data))

        if not chunk:
            raise ConnectionError("Server closed connection")

        data += chunk

    return data


def test_abnormal_disconnect():
    print("\n========== Test 1: Abnormal Disconnect ==========")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((SERVER_IP, SERVER_PORT))

    print("Client connected.")

    # 构造一帧合法注册数据，但只发送前 8 字节
    frame = make_frame(CMD_REGISTER, DEVICE_ID)

    sock.sendall(frame[:8])

    print("Sent only 8 bytes of an incomplete frame.")

    # 设置 SO_LINGER，使 close 时发送 TCP RST
    sock.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_LINGER,
        struct.pack("ii", 1, 0)
    )

    sock.close()

    print("Client closed with TCP RST.")

    # 给服务端一点时间处理断开事件
    time.sleep(1)


def test_reconnect():
    print("\n========== Test 2: New Client Reconnect ==========")

    frame = make_frame(CMD_REGISTER, DEVICE_ID)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(5)
        sock.connect((SERVER_IP, SERVER_PORT))

        print("New client connected.")

        sock.sendall(frame)

        print("Valid registration frame sent.")

        # 接收 12 字节协议头
        header_data = recv_exact(sock, 12)

        header, version, command, device_id, data_len = \
            struct.unpack("!HBBII", header_data)

        payload = recv_exact(sock, data_len)
        crc_data = recv_exact(sock, 2)

        received_crc = struct.unpack("!H", crc_data)[0]
        calculated_crc = crc16_ccitt(header_data + payload)

        if header != FRAME_HEADER:
            raise RuntimeError("Invalid response header")

        if version != PROTOCOL_VERSION:
            raise RuntimeError("Invalid response version")

        if command != CMD_RESPONSE:
            raise RuntimeError("Invalid response command")

        if device_id != DEVICE_ID:
            raise RuntimeError("Unexpected device ID")

        if received_crc != calculated_crc:
            raise RuntimeError("Response CRC error")

        if data_len != 1:
            raise RuntimeError("Unexpected response payload length")

        print(f"Device ID: {device_id}")
        print(f"Response Status: {payload[0]}")
        print("Response CRC verified.")

        if payload[0] != 0:
            raise RuntimeError("Registration did not return success")

        print("Reconnect test PASSED.")


def main():
    test_abnormal_disconnect()
    test_reconnect()


if __name__ == "__main__":
    main()

