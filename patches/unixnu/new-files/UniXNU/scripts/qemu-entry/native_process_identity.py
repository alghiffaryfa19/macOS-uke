# SPDX-License-Identifier: MIT
"""Read the active native thread's process through matching compiler offsets."""


class NativeProcessIdentity:
    def __init__(self, plan, kernel_sha256):
        self.plan = plan
        fields = ('thread_ro', 'thread_owner', 'thread_proc', 'thread_task', 'upcb',
                  'proc_ro', 'proc_owner', 'proc_task', 'task_shared_region', 'region_slide')
        if (plan.get('kernel_va_bits') != 47 or plan.get('read_only') is not True
                or plan.get('kernel_sha256') != kernel_sha256
                or any(type(plan.get('threads', {}).get(k)) is not int
                       or not 0 <= plan['threads'][k] < 65536 for k in fields)
                or any(type(plan.get('offsets', {}).get(k)) is not int
                       or not 0 <= plan['offsets'][k] < 65536 for k in ('pid', 'comm'))):
            raise ValueError('requires matching native compiler process offsets')

    def read(self, packet, thread):
        def memory(address, size):
            if not 0xffff800000000000 <= address <= (1 << 64) - size:
                raise RuntimeError('expected bounded native kernel memory')
            raw = packet(f'm{address:x},{size:x}')
            if len(raw) != size * 2 or raw.startswith('E'):
                raise RuntimeError('cannot read native thread/process linkage')
            return bytes.fromhex(raw)

        def pointer(address):
            return int.from_bytes(memory(address, 8), 'little')

        t = self.plan['threads']
        ro = pointer(thread + t['thread_ro'])
        if pointer(ro + t['thread_owner']) != thread:
            raise RuntimeError('native thread owner mismatch')
        proc = pointer(ro + t['thread_proc'])
        task = pointer(ro + t['thread_task'])
        proc_ro = pointer(proc + t['proc_ro'])
        if pointer(proc_ro + t['proc_owner']) != proc or pointer(proc_ro + t['proc_task']) != task:
            raise RuntimeError('native process/task owner mismatch')
        offsets = self.plan['offsets']
        user_state_raw = pointer(thread + t['upcb'])
        # Strip PAC metadata only for read-only address lookup; this does not authenticate the pointer.
        user_state = ((user_state_raw & ((1 << 47) - 1)) | 0xffff800000000000) if user_state_raw else 0
        region = pointer(task + t['task_shared_region'])
        slide = int.from_bytes(memory(region + t['region_slide'], 4), 'little') if region else None
        return dict(thread=hex(thread), thread_ro=hex(ro), proc=hex(proc), task=hex(task),
                    user_state=hex(user_state), user_state_raw=hex(user_state_raw),
                    pac_metadata_stripped_for_lookup_only=True,
                    pid=int.from_bytes(memory(proc + offsets['pid'], 4), 'little'),
                    name=memory(proc + offsets['comm'], 17).split(b'\0')[0].decode('ascii'),
                    shared_region=hex(region), shared_region_slide=slide)
