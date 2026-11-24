# What is open, what is sold

Hartwell OS is open core. The source in this repository is MIT licensed (see
`LICENSE`). What is **not** in this repository is the trust root and the
commercial delivery — that boundary is deliberate, and it is what keeps the
open part from eating the business.

## MIT — in this repository

Everything a person needs to read, audit, modify and rebuild the appliance:

- `src/` — the runtime (`hwo`), the interface (`fbui.c`), the PNG decoder,
  the bitmap fonts, init and the fleet agent
- `tools/` — licence tool, disk image builder, media converter, font generator
- `build.sh` and `web/` — the ISO build and the browser port of the interface
- `docs.md`, `README.md`

Rebuilding from source works and produces a complete, bootable image. Because
`keys/` is absent from the repository, the build generates a **fresh key set**
for you: your own Secure Boot chain and your own licence root. You can ship
that image under your own name; it will not accept our licence keys, and our
keys will not accept it.

## Not in this repository

| Path | Why |
|---|---|
| `keys/` | The vendor Secure Boot PK/KEK/db and the Ed25519 licence seed. The seed mints valid `HWO-YYYY-XXXX` keys for any device, so publishing it would end the paid product. |
| `dist/` | Build artifacts. Reproducible with `./build.sh`; the signed, licence-baked images are delivered to customers. |
| `buildroot/` | Cached Alpine base and kernel. Downloaded by the build, not vendored. |
| `SALES.md` | Internal sales kit: price ladder, delivery and payment terms. Published on request, not in the public tree. |

History is squashed at the first public release: the seed has existed for a
while, and it was never in a published commit.

## Sold

What actually gets paid for:

- **A licence key**, bound to the machine's DMI UUID, verified offline.
  One machine, perpetual, transfers with the hardware.
- **A signed image with the licence baked in** — no activation step on the
  floor, no key to type.
- **Updates for 12 months**, built and signed by us.
- **Support** on the appliance, and the ability to say "someone is answerable
  for this box" in a tender.

An image without a licence boots into the activation screen and stays there.
That is the whole gate — there is no time bomb, no phone-home, nothing that
breaks a customer's machine if a payment is late.

## Trademark

The MIT licence covers code, not names. A fork may not present itself as
Hartwell OS, and may not use the Hartwell Labs marks, without written
permission. Describe it as "based on Hartwell OS" and link back — that is
enough.

## Security

Report issues to `business@hartwell-labs.pl` or via
<https://hartwell-labs.pl/security/>. Please do not open a public issue for a
vulnerability before we have had a chance to ship a fix.
