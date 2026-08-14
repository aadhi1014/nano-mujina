#!/usr/bin/env python3
"""Computes a Stratum merkle root from a coinbase transaction and merkle branch."""
import hashlib

def sha256d(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()

coinbase_hex = (
    "02000000010000000000000000000000000000000000000000000000000000000000"
    "000000ffffffff4c03a6ab0e01000cb31fa73f000000000000000020ed07de03b56072"
    "f1568ba9a6658f040d72a617e0835f192b6c9a2005a79dd6920842f5d399c08cc9180e"
    "6879647261706f6f6c2f32353666feffffff028ddbd51200000000160014c64b1b928"
    "3ba1ea86bb9e7b696b0c8f68dad04000000000000000000266a24aa21a9ed71fa5bcc"
    "1109b9b40314d9cda01046d861c62628afdee40f78f3fbb813913c24a5ab0e00"
)
coinbase = bytearray.fromhex(coinbase_hex)

nonce2 = 0x00000000
nonce2_offset = 53
nonce2_size = 4
coinbase[nonce2_offset:nonce2_offset + nonce2_size] = nonce2.to_bytes(nonce2_size, "little")

merkles_hex = [
    "da588b806f7b30fe73074595a8b6e11523a2b8d1abe450fa18e8241c207381f1"[-64:],
    "390b2e944073a5c5e22760c2737291e86f765a5bacc5b49385f9d7637a1c52f3"[-64:],
    "c65e493a2f4e6a85620f348ec111afe61890c98bfbbe97ac954c6cbeea8233e3"[-64:],
    "f8becda94d22e071386328f529f55eef275a1027072e785f761f53ae29b78838"[-64:],
    "5e9d68c8eb3f1a2d524c77d1c51fe91a8b88c49c1c267b7583ecb5664ed47433"[-64:],
    "68d8cb1697eacb8a8e2c329af2a1afc257327938589282a136178cf19bf0eba0"[-64:],
    "216a9904e50915be9f80aa92e91e84dd70e9fe618d20be4b1ee88900349dfc5b"[-64:],
    "fd15b44cf384adf6837bd953e1a88c38394fd15476a5e8472f3107a94580a1aa"[-64:],
    "ca98149e4ddcf732af00b9ec0d0ffd545b95e803a2f2614881e36a5b74b3af6a"[-64:],
    "765ab614be7cf654400fc5bc0e2b72f2270cbc7088b416a2132cf359ddc6c09e"[-64:],
    "8db01f6654ebd278b480de6aff17c3e763224370c0780412bbed195f9a7c758c"[-64:],
    "b36f0e20b2c24f61b01cbae59a06e69a7e3078f35003e52e27d470c1c0326e10"[-64:],
    "e0e5d87953a0e1ad9d5c4ee566210a126da60e769603b29956164de0d73b33b0"[-64:],
]
merkles = [bytes.fromhex(h) for h in merkles_hex]

running = sha256d(bytes(coinbase))
for branch in merkles:
    running = sha256d(running + branch)

# final word-wise byte-swap: reverse each 4-byte word in place
swapped = bytearray(32)
for w in range(8):
    swapped[w*4:w*4+4] = running[w*4:w*4+4][::-1]

print("python_reference_root=" + swapped.hex())
