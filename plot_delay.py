#!/usr/bin/env python3
"""
Script para graficar el one-way delay en función del número de medición
para diferentes condiciones de red simuladas.
"""

import csv
import os
from statistics import mean, stdev

import matplotlib.pyplot as plt

# Configuración de estilo
plt.rcParams["figure.facecolor"] = "#1a1a2e"
plt.rcParams["axes.facecolor"] = "#16213e"
plt.rcParams["axes.edgecolor"] = "#e94560"
plt.rcParams["axes.labelcolor"] = "#eaeaea"
plt.rcParams["text.color"] = "#eaeaea"
plt.rcParams["xtick.color"] = "#eaeaea"
plt.rcParams["ytick.color"] = "#eaeaea"
plt.rcParams["axes.grid"] = True
plt.rcParams["grid.color"] = "#0f3460"
plt.rcParams["grid.alpha"] = 0.6

# Directorio con las mediciones
mediciones_dir = "mediciones"
plots_dir = "plots"

# Crear directorio de plots si no existe
os.makedirs(plots_dir, exist_ok=True)


def leer_csv(filepath):
    """Lee un archivo CSV y devuelve las columnas measurement y one_way_delay_us"""
    measurements = []
    delays = []
    with open(filepath, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            measurements.append(int(row["measurement"]))
            delays.append(float(row["one_way_delay_us"]))
    return measurements, delays


# Archivos CSV y sus etiquetas
archivos = {
    "normal.csv": "Normal (sin pérdida/delay)",
    "loss1.csv": "Pérdida 1%",
    "loss2.csv": "Pérdida 2%",
    "loss5.csv": "Pérdida 5%",
    "delayjitter.csv": "Delay + Jitter",
}

# Colores para cada condición
colores = {
    "normal.csv": "#00d9ff",
    "loss1.csv": "#7dff7d",
    "loss2.csv": "#ffcc00",
    "loss5.csv": "#ff6b6b",
    "delayjitter.csv": "#bf7dff",
}

# Crear figura con subplots
fig, axes = plt.subplots(3, 2, figsize=(14, 12))
fig.suptitle(
    "One-Way Delay vs Número de Medición\nDiferentes Condiciones de Red",
    fontsize=16,
    fontweight="bold",
    color="#e94560",
)

axes = axes.flatten()

# Leer y graficar cada archivo
datos = {}
for i, (archivo, etiqueta) in enumerate(archivos.items()):
    filepath = os.path.join(mediciones_dir, archivo)
    if os.path.exists(filepath):
        measurements, delays = leer_csv(filepath)
        datos[archivo] = (measurements, delays)

        ax = axes[i]
        ax.plot(measurements, delays, color=colores[archivo], linewidth=0.5, alpha=0.8)
        ax.set_title(etiqueta, fontsize=12, fontweight="bold", color=colores[archivo])
        ax.set_xlabel("Número de Medición", fontsize=10)
        ax.set_ylabel("One-Way Delay (μs)", fontsize=10)

        # Añadir estadísticas
        media = mean(delays)
        desv = stdev(delays) if len(delays) > 1 else 0
        ax.axhline(y=media, color="#e94560", linestyle="--", linewidth=1.5, alpha=0.7)
        ax.text(
            0.02,
            0.98,
            f"μ = {media:.3f} μs\nσ = {desv:.3f} μs",
            transform=ax.transAxes,
            fontsize=9,
            verticalalignment="top",
            bbox=dict(
                boxstyle="round", facecolor="#1a1a2e", edgecolor="#e94560", alpha=0.8
            ),
        )

# Ocultar el subplot vacío
axes[5].set_visible(False)

plt.tight_layout()
plt.savefig(
    os.path.join(plots_dir, "one_way_delay_subplots.png"),
    dpi=150,
    facecolor="#1a1a2e",
    edgecolor="none",
    bbox_inches="tight",
)
print("Guardado: plots/one_way_delay_subplots.png")

# Crear también un gráfico combinado
fig2, ax2 = plt.subplots(figsize=(14, 8))
fig2.patch.set_facecolor("#1a1a2e")

for archivo, etiqueta in archivos.items():
    if archivo in datos:
        measurements, delays = datos[archivo]
        ax2.plot(
            measurements,
            delays,
            color=colores[archivo],
            linewidth=0.6,
            alpha=0.7,
            label=etiqueta,
        )

ax2.set_title(
    "Comparación de One-Way Delay\nTodas las Condiciones de Red",
    fontsize=16,
    fontweight="bold",
    color="#e94560",
)
ax2.set_xlabel("Número de Medición", fontsize=12)
ax2.set_ylabel("One-Way Delay (μs)", fontsize=12)
ax2.legend(
    loc="upper right",
    facecolor="#16213e",
    edgecolor="#e94560",
    labelcolor="#eaeaea",
    fontsize=10,
)

plt.tight_layout()
plt.savefig(
    os.path.join(plots_dir, "one_way_delay_combined.png"),
    dpi=150,
    facecolor="#1a1a2e",
    edgecolor="none",
    bbox_inches="tight",
)
print("Guardado: plots/one_way_delay_combined.png")

plt.show()

print("\n¡Gráficos generados exitosamente!")
print("\n¡Gráficos generados exitosamente!")
