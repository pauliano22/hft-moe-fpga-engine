# FPGA-Accelerated Sparse MoE Trading Engine

## 1. Project Vision
This project implements an ultra-low latency, hardware-accelerated trading engine capable of ingesting raw **NASDAQ ITCH 5.0** market data and executing trade signals based on a **Sparse Mixture of Experts (MoE)** machine learning model. 

The core objective is to bridge the gap between high-level algorithmic capacity and the physical limits of hardware, achieving **deterministic, sub-microsecond wire-to-response latency**.

---

## 2. Targeted Performance Metrics
To compete at the level of firms like HRT, Citadel, and Optiver, we are aiming for the following "Head-Turning" benchmarks:

| Metric | Target | Proof Method |
| :--- | :--- | :--- |
| **Wire-to-Response Latency** | < 100ns | Cycle-accurate Verilator trace |
| **MoE Inference Latency** | < 500ns | Vitis HLS Synthesis Report |
| **Peak Throughput** | 150M+ msg/s | Stress-test simulation logs |
| **Jitter (Tail Latency)** | 0ns (Deterministic) | Pipeline analysis in HLS |
| **Data Integrity** | 100% Bit-Accuracy | Verilator vs. C++ Golden Model |

---

## 3. The Technical Stack & Tooling
We utilize an industrial-grade toolchain to ensure the system is production-ready.

### Hardware & Simulation (The "Silicon" Layer)
* **SystemVerilog:** Used for the high-speed binary packet parser and AXI-Stream interfaces.
* **Vitis HLS (C++):** Used to design the MoE Router and Matching Engine logic.
* **Vivado ML:** For hardware synthesis and timing closure analysis.
* **Verilator:** Our primary simulation engine for high-speed, cycle-accurate verification.

### Verification & Profiling (The "Rigor" Layer)
* **GDB:** For step-through debugging of the C-Simulation logic.
* **Valgrind:** To guarantee zero memory leaks in the host-side simulation and golden model.
* **Linux Perf:** To profile simulation overhead and optimize the data-feeding pipeline.
* **GTKWave:** To visualize hardware signal waveforms and identify nanosecond-level timing issues.

---

## 4. Visual Evidence (Screenshots to Include)
A "Wow" project requires visual proof. We will populate the `/docs/images` folder with the following:

1.  **[Waveform] ITCH Packet Decoding:** A GTKWave screenshot showing raw binary data entering the FPGA and being "cut" into Price/Side/Quantity signals.
2.  **[HLS Report] Pipelining & Resource Usage:** A screenshot from the Vitis HLS GUI showing a "Trip Count" of 1 and an "Initiation Interval (II)" of 1, proving we process one order every clock cycle.
3.  **[Flame Graph] Simulation Efficiency:** A Linux Perf flame graph showing that 99% of our CPU time is spent in the hardware logic, not software overhead.
4.  **[Valgrind] Memory Safety:** A clean "Definitively Lost: 0 bytes" report from Valgrind to prove industrial stability.
5.  **[Architecture] Hardware Diagram:** A high-level block diagram showing the data flow from the 10GbE MAC to the MoE Experts.

---

## 5. System Architecture



### A. The "Shell" (SystemVerilog)
A line-rate parser that monitors the 64-bit AXI-Stream. It identifies the "Add Order" (Type 'A') message byte-by-byte, extracting data in real-time without buffering full packets.

### B. The "Brain" (Vitis HLS / Sparse MoE)
* **Router:** A hardware-based router that assigns market features to specialized "Experts."
* **Experts:** Small, hardware-optimized MLP kernels implemented in HLS.
* **Matching Engine:** A deterministic Limit Order Book (LOB) utilizing **BRAM Partitioning** for $O(1)$ memory access.

### C. The Verification Pipeline
1.  **Golden Model:** A C++ reference implementation.
2.  **Verilator Simulation:** Transpiles Verilog/HLS to C++ for bit-accurate testing.
3.  **Automated Checker:** A script that compares every trade decision between hardware and software.

---

## 6. Project Roadmap
- [ ] **Day 1-2:** Build C++ Golden Model and parse raw NASDAQ PCAP files.
- [ ] **Day 3-4:** Implement HLS Matching Engine (The "Brain").
- [ ] **Day 5:** Develop SystemVerilog Parser (The "Shell").
- [ ] **Day 6:** Integrate MoE Router and Expert kernels.
- [ ] **Day 7:** Run full Verilator simulation and generate performance reports.

---
*This project is a demonstration of bridging the gap between hardware architecture and quantitative finance.*
