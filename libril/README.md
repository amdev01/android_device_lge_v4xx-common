# Device libril (KitKat QCRIL CardStatus)

Fork of Lineage `hardware/ril` **cm-14.1-caf** `libril`, enabled via
`BOARD_PROVIDES_LIBRIL := true`.

Stock LG KitKat `libril-qc-qmi-1.so` returns `RIL_AppStatus` with **four**
extra retry ints after `pin2` (`pin1`/`puk1`/`pin2`/`puk2` num_retries).
Measured on v490: `responselen=408` ≡ `sizeof(RIL_CardStatus_v6)` with
`AppStatus=48`. AOSP/CAF without those fields (or with a fifth
`perso_unblock_retries`) mismatches and `responseSimStatus` returns
`INVALID_RESPONSE` → SIM `NOT_READY`.

Retry fields are not written to the Java parcel (framework stays stock).
