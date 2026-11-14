# Device libril (KitKat QCRIL ABI)

Fork of Lineage `hardware/ril` **cm-14.1-caf** `libril`, enabled via
`BOARD_PROVIDES_LIBRIL := true`.

## CardStatus (`GET_SIM_STATUS`)

Stock LG KitKat `libril-qc-qmi-1.so` returns `RIL_AppStatus` with **four**
extra retry ints after `pin2` (`pin1`/`puk1`/`pin2`/`puk2` num_retries).
Measured on v490: `responselen=408` ≡ `sizeof(RIL_CardStatus_v6)` with
`AppStatus=48`. AOSP/CAF without those fields (or with a fifth
`perso_unblock_retries`) mismatches and `responseSimStatus` returns
`INVALID_RESPONSE` → SIM `NOT_READY`.

Retry fields are not written to the Java parcel (framework stays stock).

## SIM_IO (`RIL_REQUEST_SIM_IO`)

Stock LG KitKat QCRIL expects `RIL_SIM_IO_v6` with a leading `int cla`
(sizeof **40** on 32-bit). CAF/AOSP is 36 bytes (`command` at offset 0).
Without `cla`, QCRIL reads `fileid` as `command` → `ILLEGAL: unknown cmd` →
`REQUEST_NOT_SUPPORTED` for every EF read (ICCID null → no SubInfo → UI
“No SIM”). `dispatchSIM_IO` sets `cla=0` (not in the RILJ parcel).
