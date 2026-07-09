# HPCA 2027 iFuse Power and Register-File Methodology

## Task A: iFuse Power Estimation

Use Scarab's existing McPAT/CACTI power flow for the no-fusion baseline, then add iFuse-specific hardware as explicit delta components. The baseline configuration is the `hpca2027-baseline` Scarab branch with `PARAMS.in` using `--dcache_assoc 8`. Keep power-control flags in scarab-infra JSON descriptors, not in `PARAMS.in`:

```text
--power_intf_on 1 --power_intf_enable_scaling 0
```

For each new iFuse hardware structure, estimate dynamic and leakage energy separately:

```text
E_dynamic = reads * E_read + writes * E_write + searches * E_search
E_leakage = P_leakage * simulated_time
P_ifuse_total = (E_baseline + E_FCT + E_APT + E_predictors) / simulated_time
```

Model FCT, APT, and any predictor tables with CACTI/McPAT-style parameters wherever possible: number of entries, entry width, read/write/search ports, banking/associativity, and update policy. If a structure does not map cleanly to McPAT, keep it as a separately reported delta using the same energy equations.

Required Scarab counters for final power numbers:

- FCT lookup, hit, miss, insert, update, evict, and confidence-update events.
- APT lookup, hit, miss, insert, update, evict, and match-policy events.
- Predictor lookup/update events for any auxiliary iFuse predictor structures.
- Optional read/write/search splits when a structure has asymmetric access energy.

Report both absolute power and delta-over-baseline power so the contribution of FCT/APT/predictors is visible.

## Task B: Register-File Utilization

Measure physical register pressure by sampling occupancy once per cycle:

```text
occupied_regs = physical_reg_file_size - free_regs
utilization_pct = occupied_regs / physical_reg_file_size * 100
```

The implementation adds cumulative Scarab stats:

- `REG_FILE_UTIL_SAMPLES`
- `REG_FILE_INT_PHYS_REGS_SUM`
- `REG_FILE_VEC_PHYS_REGS_SUM`
- `REG_FILE_INT_OCCUPIED_SUM`
- `REG_FILE_VEC_OCCUPIED_SUM`

Post-process average utilization as:

```text
int_rf_util_pct = REG_FILE_INT_OCCUPIED_SUM / REG_FILE_INT_PHYS_REGS_SUM * 100
vec_rf_util_pct = REG_FILE_VEC_OCCUPIED_SUM / REG_FILE_VEC_PHYS_REGS_SUM * 100
```

Use the same stats for no-fusion baseline and fusion runs. Do not use final occupancy as the utilization metric; it depends on drain state and does not represent register pressure over the run.

On the current HPCA baseline, the configured physical register-file sizes are:

- Integer physical RF: 280 registers.
- Vector physical RF: 332 registers.
