# Security policy

## Reporting a vulnerability

Report it privately through GitHub's private vulnerability reporting: the **Security** tab of this repository,
then **Report a vulnerability**. Please do not open a public issue for a security problem.

This covers the code in this fork: the `ddk113` branch and its tools. Problems in upstream Mesa belong with
the [Mesa project](https://docs.mesa3d.org/), and problems in the original img-pvr belong with
[JimVulkan/img-pvr](https://github.com/JimVulkan/img-pvr).

## Scope notes

- The driver runs inside unprivileged app processes and talks to the vendor GPU kernel driver through the DRM
  render node. Any input that reaches a kernel ioctl is in scope.
- Development tooling that needs root (a planned ptrace-based bridge tracer under `tools/`) is never shipped
  in release archives.
- This repository never contains proprietary vendor binaries, raw captures of vendor driver output,
  credentials, or device-identifying data. Please report any that slip in.
