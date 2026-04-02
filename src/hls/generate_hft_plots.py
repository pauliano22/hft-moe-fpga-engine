import matplotlib.pyplot as plt
import numpy as np

# Set a professional dark-mode friendly style
plt.rcParams.update({
    "lines.color": "white",
    "patch.edgecolor": "white",
    "text.color": "white",
    "axes.facecolor": "#1c1c1c",
    "axes.edgecolor": "lightgray",
    "axes.labelcolor": "white",
    "xtick.color": "white",
    "ytick.color": "white",
    "grid.color": "#333333",
    "figure.facecolor": "#1c1c1c",
    "figure.edgecolor": "#1c1c1c",
    "savefig.facecolor": "#1c1c1c",
    "savefig.edgecolor": "#1c1c1c"})

# --- CHART 1: LATENCY WATERFALL ---
# Real cycles from your reports: LOB(42), Router(65), Expert(4)
# Total = 111 cycles @ 4ns clock = 444ns
modules = ['LOB Update', 'MoE Routing', 'Expert Inference']
latencies = [168, 260, 16] # in nanoseconds
cumulative = np.cumsum([0] + latencies[:-1])

fig, ax = plt.subplots(figsize=(10, 6))
colors = ['#4CC9F0', '#4361EE', '#3A0CA3']

for i in range(len(modules)):
    ax.bar(modules[i], latencies[i], bottom=cumulative[i], color=colors[i], edgecolor='white')
    # Add labels
    ax.text(i, cumulative[i] + latencies[i]/2, f'{latencies[i]}ns', 
            ha='center', va='center', fontweight='bold', color='white')

# Total bar
ax.bar(['Total Engine'], [444], color='#F72585', alpha=0.8, edgecolor='white')
ax.text(3, 222, '444ns', ha='center', va='center', fontweight='bold', color='white', fontsize=12)

ax.set_ylabel('Latency (Nanoseconds)')
ax.set_title('Wire-to-Decision Timing Budget', fontsize=16, fontweight='bold', pad=20)
plt.grid(axis='y', linestyle='--', alpha=0.3)
plt.savefig('latency_waterfall.png', dpi=300)
print("Saved: latency_waterfall.png")

# --- CHART 2: THROUGHPUT SCALABILITY ---
# 1.7M (Soft) vs 83.3M (Hard)
plt.figure(figsize=(8, 6))
labels = ['C++ Software\n(Thread-Bound)', 'FPGA MoE Engine\n(Clock-Aligned)']
throughput = [1.7, 83.3]
bars = plt.bar(labels, throughput, color=['#707070', '#4CC9F0'], edgecolor='white')

plt.yscale('log')
plt.ylabel('Million Messages Per Second (Log Scale)')
plt.title('Throughput: 50x Hardware Acceleration', fontsize=16, fontweight='bold', pad=20)

for bar in bars:
    yval = bar.get_height()
    plt.text(bar.get_x() + bar.get_width()/2, yval, f'{yval}M', 
             va='bottom', ha='center', fontweight='bold', fontsize=12)

plt.savefig('throughput_comparison.png', dpi=300)
print("Saved: throughput_comparison.png")

# --- CHART 3: RESOURCE RADAR ---
# Data from your reports: 32 BRAM, 4 DSP, ~2% LUT, ~1% FF
res_labels = ['BRAM', 'DSP', 'LUT', 'FF']
# Percentage of XCVU9P
res_usage = [0.74, 0.05, 2.6, 0.9] 

plt.figure(figsize=(8, 6))
plt.barh(res_labels, res_usage, color='#7209B7', edgecolor='white')
plt.xlabel('Chip Utilization (%)')
plt.title('Xilinx Virtex UltraScale+ Footprint', fontsize=16, fontweight='bold', pad=20)
plt.xlim(0, 5) # To show how much massive headroom is left

for i, v in enumerate(res_usage):
    plt.text(v + 0.1, i, f'{v}%', va='center', fontweight='bold')

plt.savefig('resource_footprint.png', dpi=300)
print("Saved: resource_footprint.png")
