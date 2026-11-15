# pco-scan

Finds and decodes PCO instruction-group streams inside arbitrary binary blobs. The tables are not
copied: `isa_tables.py` imports `src/imagination/pco/pco_isa.py` (via `pco_pygen_common.py`) and builds
a bit-field matcher for every instruction encoding it defines.

- `pco_scan.py FILE...` header-chain scan (group lengths must chain up to a group with the `end` bit),
  then decode of each group's instruction phases (op names and modifiers). Source/destination operand
  fields are not decoded.
- `test_scan.py` decodes a PCO-compiled fragment shader and scans it back out of a padded buffer.

Run with `python3 -I tools/pco-scan/pco_scan.py FILE` (needs only the Python standard library).
Groups the ISA tables cannot describe print as `UNDECODED`.
