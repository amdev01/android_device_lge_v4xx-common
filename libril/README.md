# Device libril (KitKat QCRIL CardStatus)

Fork of Lineage `hardware/ril` **cm-14.1-caf** `libril`, enabled via
`BOARD_PROVIDES_LIBRIL := true`.

Stock LG KitKat `libril-qc-qmi-1.so` returns `RIL_AppStatus` with five extra
retry ints after `pin2`. AOSP/CAF `sizeof(RIL_CardStatus_v5/v6)` did not match,
so `responseSimStatus` returned `INVALID_RESPONSE` and SIM stayed `NOT_READY`.

This tree extends `RIL_AppStatus` accordingly. Retry fields are not written to
the Java parcel (framework stays stock).
