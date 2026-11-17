import socket
import struct

SERVER_IP = "127.0.0.1"
SERVER_PORT = 8080

FRAME_HEADER = 0xAA55
PROTOCOL_VERSION = 0x01
CMD_REGISTER = 0x01
DEVICE_ID = 3001


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


def build_frame(command, device_id):
    header = struct.pack(
        "!HBBII",
        FRAME_HEADER,
        PROTOCOL_VERSION,
        command,
        device_id,
        0
    )

    crc = crc16_ccitt(header)

    return header + struct.pack("!H", crc)


def recv_exact(sock, length):
    data = b""

    while len(data) < length:
        chunk = sock.recv(length - len(data))

        if not chunk:
            raise ConnectionError("Server closed the connection")

        data += chunk

    return data


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)

    try:
        sock.connect((SERVER_IP, SERVER_PORT))
        print("TCP connection established.")

        # 构造正常注册帧
        valid_frame = build_frame(CMD_REGISTER, DEVICE_ID)

        # 构造CRC错误的注册帧
        bad_frame = bytearray(valid_frame)
        bad_frame[-1] ^= 0x01
        bad_frame = bytes(bad_frame)

        print("\nSending corrupted CRC frame...")
        print("Bad frame:", bad_frame.hex(" "))

        print("\nSending valid registration frame...")
        print("Valid frame:", valid_frame.hex(" "))

        # 一次发送两帧，测试解析器恢复能力
        sock.sendall(bad_frame + valid_frame)

        # 接收响应头
        response_header = recv_exact(sock, 12)

        header, version, command, device_id, data_len = struct.unpack(
            "!HBBII",
            response_header
        )

        if header != FRAME_HEADER:
            raise ValueError("Invalid response header")

        if version != PROTOCOL_VERSION:
            raise ValueError("Invalid response version")

        if command != 0x81:
            raise ValueError(
                f"Unexpected response command: 0x{command:02X}"
            )

        if device_id != DEVICE_ID:
            raise ValueError("Unexpected device ID")

        if data_len > 1024:
            raise ValueError("Response payload too large")

        remaining = recv_exact(sock, data_len + 2)

        payload = remaining[:data_len]
        received_crc = struct.unpack("!H", remaining[data_len:])[0]

        calculated_crc = crc16_ccitt(response_header + payload)

        if received_crc != calculated_crc:
            raise ValueError("Response CRC verification failed")

        print("\n========== Server Response ==========")
        print(f"Device ID: {device_id}")
        print(f"Command: 0x{command:02X}")
        print(f"Payload Length: {data_len}")
        print("Payload:", payload.hex(" "))

        if data_len >= 1:
            print(f"Registration Status: {payload[0]}")

        print("Response CRC: OK")

    except Exception as e:
        print("Test failed:", e)

    finally:
        sock.close()
        print("\nConnection closed.")


if __name__ == "__main__":
    main()
