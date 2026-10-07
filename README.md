# FPLinux

FPLinux is a source-built Linux port for selected feature phones. It loads into
volatile RAM without flashing, erasing or writing the phone's internal storage.
Writes to supported microSD storage require the documented shutdown procedure.

## Documentation

[The FPLinux website](https://fplinux.github.io/fplinux/) owns the user guides,
phone support checklists, developer contracts and code style.

- [Build FPLinux](https://fplinux.github.io/fplinux/start/build/)
- [Run FPLinux](https://fplinux.github.io/fplinux/start/run/)
- [Phone support](https://fplinux.github.io/fplinux/phones/)
- [Contributing](https://fplinux.github.io/fplinux/develop/contributing/)
- [Bring up a new phone](https://fplinux.github.io/fplinux/porting/bring-up/)

Documentation sources live in `site/src/content/docs/`.
To preview them, use the [site workflow](https://fplinux.github.io/fplinux/develop/contributing/#preview-this-site).

## Source layout

The [porting overview](https://fplinux.github.io/fplinux/porting/) explains the
source layers and their responsibilities. Phone-specific wiring and register
data remain with their target.

## License

Original FPLinux code and documentation use [GPL-2.0-only](LICENSE) unless a
file states otherwise. Third-party components retain their licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
