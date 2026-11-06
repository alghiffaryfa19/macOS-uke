"""Read the versioned SHENG ring while QEMU is stopped; never change guest state."""
import json
import struct
import uuid


class RetainedLogStream:
    HEADER, PREFIX, TAIL = 128, 16384, 65536

    def __init__(self, base, identity, directory):
        self.base = base
        self.identity = uuid.UUID(identity).bytes
        self.directory = directory
        self.next = 0
        self.samples = 0
        self.missing = 0
        self.dropped = 0
        self.ready = False
        (directory / 'stream-console.bin').write_bytes(b'')
        (directory / 'stream-console.jsonl').write_text('')

    def sample(self, packet, elapsed):
        # A fixed total budget keeps capture bounded even with many tracepoints
        if self.samples >= 256:
            return
        raw = bytearray()
        size = self.HEADER + self.PREFIX + self.TAIL
        for offset in range(0, size, 4096):
            length = min(4096, size - offset)
            value = packet(f'm{self.base + offset:x},{length:x}')
            if value.startswith('E'):
                if not self.ready:
                    return
                raise RuntimeError('Previously mapped retained log became unreadable')
            if len(value) != length * 2:
                raise RuntimeError('Incomplete retained log memory response')
            raw.extend(bytes.fromhex(value))
        if raw[:8] != b'SHNLOG01':
            if not self.ready:
                return
            raise RuntimeError('Retained log signature changed during observation')
        if struct.unpack_from('<4I', raw, 8) != (1, self.HEADER, self.PREFIX, self.TAIL):
            raise RuntimeError('Unsupported retained log layout')
        if raw[112:128] != self.identity:
            raise RuntimeError('Retained log UUID differs from this kernel')
        total, dropped, writer, panic = struct.unpack_from('<QQII', raw, 24)
        if writer not in (0, 1) or panic not in (0, 1, 2, 3):
            raise RuntimeError('Invalid retained log writer state')
        if total < self.next:
            raise RuntimeError('Retained log counter moved backwards')
        self.ready = True
        chunks = []
        cursor = self.next
        if cursor < min(total, self.PREFIX):
            end = min(total, self.PREFIX)
            chunks.append((cursor, bytes(raw[self.HEADER + cursor:self.HEADER + end])))
            cursor = end
        oldest = max(min(total, self.PREFIX), total - self.TAIL)
        if writer and total >= self.TAIL and oldest == total - self.TAIL:
            oldest += 1
        gap = max(0, oldest - cursor)
        cursor = max(cursor, oldest)
        if cursor < total:
            ring = raw[self.HEADER + self.PREFIX:]
            start = cursor % self.TAIL
            chunks.append((cursor, bytes((ring[start:] + ring[:start])[:total - cursor])))
        with (self.directory / 'stream-console.bin').open('ab') as stream:
            if gap:
                stream.write(f'\n[... {gap} committed bytes missed between samples ...]\n'.encode())
            for _, data in chunks:
                stream.write(data)
            if dropped > self.dropped:
                stream.write(f'\n[... {dropped - self.dropped} bytes dropped by guest writer ...]\n'.encode())
        record = dict(elapsed_seconds=round(elapsed, 3), start=self.next, end=total,
                      missing_bytes=gap, guest_dropped_bytes=dropped,
                      writer_active=bool(writer), panic_state=panic,
                      chunks=[dict(start=at, bytes=len(data)) for at, data in chunks])
        with (self.directory / 'stream-console.jsonl').open('a') as stream:
            stream.write(json.dumps(record) + '\n')
        self.next, self.dropped = total, dropped
        self.samples += 1
        self.missing += gap

    def report(self):
        return dict(samples=self.samples, total_bytes=self.next, missing_bytes=self.missing,
                    guest_dropped_bytes=self.dropped, ready=self.ready, read_only=True,
                    complete=self.ready and not (self.missing or self.dropped),
                    periodic_debugger_interrupts=True, sample_limit=256)
