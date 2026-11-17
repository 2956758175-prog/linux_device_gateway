
import socket
import struct

SERVER_IP = "192.168.92.129"
SERVER_PORT = 8080
DEVICE_ID = 3004

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


def make_frame(version, command, device_id, payload=b""):
    header = struct.pack(
        "!HBBII",
        FRAME_HEADER,
        version,
        command,
        device_id,
        len(payload)
    )

    frame = header + payload
    crc = crc16_ccitt(frame)

    return frame + struct.pack("!H", crc)


def make_bad_crc_frame():
    frame = bytearray(
        make_frame(
            PROTOCOL_VERSION,
            CMD_REGISTER,
            DEVICE_ID
        )
    )

    # 修改 CRC，使校验失败
    frame[-1] ^= 0xFF

    return bytes(frame)


def recv_exact(sock, length):
    data = b""

    while len(data) < length:
        chunk = sock.recv(length - len(data))

        if not chunk:
            raise ConnectionError("Server closed connection")

        data += chunk

    return data


def receive_response(sock):
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
        raise RuntimeError(
            f"Unexpected response command: 0x{command:02X}"
        )

    if device_id != DEVICE_ID:
        raise RuntimeError("Unexpected device ID")

    if received_crc != calculated_crc:
        raise RuntimeError("Response CRC check failed")

    if data_len != 1:
        raise RuntimeError(
            f"Unexpected response payload length: {data_len}"
        )

    print("Response received successfully.")
    print(f"Device ID: {device_id}")
    print(f"Response Status: {payload[0]}")

    if payload[0] != 0:
        raise RuntimeError("Device registration failed")

    print("Response CRC verified.")
    print("Consecutive bad-frame recovery test PASSED.")


def main():
    # 1. 错误版本帧
    bad_version = make_frame(
        0x02,
        CMD_REGISTER,
        DEVICE_ID
    )

    # 2. 超长 Payload 帧
    bad_length_header = struct.pack(
        "!HBBII",
        FRAME_HEADER,
        PROTOCOL_VERSION,
        CMD_REGISTER,
        DEVICE_ID,
        2048
    )

    bad_length_crc = crc16_ccitt(bad_length_header)

    bad_length = (
        bad_length_header +
        struct.pack("!H", bad_length_crc)
    )

    # 3. CRC 错误帧
    bad_crc = make_bad_crc_frame()

    # 4. 正常注册帧
    valid_frame = make_frame(
        PROTOCOL_VERSION,
        CMD_REGISTER,
        DEVICE_ID
    )

    print("Connecting to server...")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(5)
        sock.connect((SERVER_IP, SERVER_PORT))

        print("TCP connection established.")

        test_data = (
            bad_version +
            bad_length +
            bad_crc +
            valid_frame
        )

        print(f"Sending {len(test_data)} bytes...")
        print("1. Invalid protocol version")
        print("2. Payload length exceeds maximum")
        print("3. Invalid CRC")
        print("4. Valid registration frame")

        sock.sendall(test_data)

        receive_response(sock)


if __name__ == "__main__":
    main()

