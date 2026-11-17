
import socket
import struct

SERVER_IP = "127.0.0.1"
SERVER_PORT = 8080

FRAME_HEADER = 0xAA55
CMD_REGISTER = 0x01
DEVICE_ID = 3002


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


def build_frame(version, command, device_id):
    header = struct.pack(
        "!HBBII",
        FRAME_HEADER,
        version,
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
            raise ConnectionError("Server closed connection")

        data += chunk

    return data


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)

    try:
        sock.connect((SERVER_IP, SERVER_PORT))
        print("TCP connection established.")

        # 错误版本号：0x02
        bad_frame = build_frame(
            0x02, CMD_REGISTER, DEVICE_ID
        )

        # 正确版本号：0x01
        valid_frame = build_frame(
            0x01, CMD_REGISTER, DEVICE_ID
        )

        print("Sending invalid version frame:")
        print(bad_frame.hex(" "))

        print("\nSending valid version frame:")
        print(valid_frame.hex(" "))

        # 连续发送两帧
        sock.sendall(bad_frame + valid_frame)

        # 接收响应
        response_header = recv_exact(sock, 12)

        header, version, command, device_id, data_len = (
            struct.unpack("!HBBII", response_header)
        )

        if header != FRAME_HEADER:
            raise ValueError("Invalid response header")

        if version != 0x01:
            raise ValueError("Invalid response version")

        if command != 0x80:
            raise ValueError(
                f"Unexpected command: 0x{command:02X}"
            )

        if device_id != DEVICE_ID:
            raise ValueError("Unexpected device ID")

        if data_len > 1024:
            raise ValueError("Invalid payload length")

        remaining = recv_exact(sock, data_len + 2)

        payload = remaining[:data_len]
        received_crc = struct.unpack(
            "!H", remaining[data_len:]
        )[0]

        calculated_crc = crc16_ccitt(
            response_header + payload
        )

        if received_crc != calculated_crc:
            raise ValueError("Response CRC error")

        print("\n========== Response ==========")
        print(f"Device ID: {device_id}")
        print(f"Command: 0x{command:02X}")
        print(f"Payload Length: {data_len}")
        print(f"Payload: {payload.hex(' ')}")
        print("Response CRC: OK")

        if data_len > 0:
            print(f"Registration Status: {payload[0]}")

        print("\nTEST PASSED")

    except Exception as e:
        print("\nTEST FAILED:", e)

    finally:
        sock.close()


if __name__ == "__main__":
    main()

