#!/usr/bin/env python3
"""Quick verification of CNN model behavior with idle-like inputs."""
import torch
import torch.nn as nn
import numpy as np
import json
import os
os.environ["KMP_DUPLICATE_LIB_OK"] = "TRUE"

class IMU1DCNN(nn.Module):
    def __init__(self, nc, ncl, ws=25):
        super().__init__()
        self.conv1 = nn.Conv1d(nc, 32, 7, padding=3)
        self.bn1 = nn.BatchNorm1d(32)
        self.conv2 = nn.Conv1d(32, 64, 5, padding=2)
        self.bn2 = nn.BatchNorm1d(64)
        self.conv3 = nn.Conv1d(64, 64, 3, padding=1)
        self.bn3 = nn.BatchNorm1d(64)
        self.pool = nn.AdaptiveAvgPool1d(1)
        self.dropout = nn.Dropout(0.3)
        self.fc = nn.Linear(64, ncl)

    def forward(self, x):
        x = torch.relu(self.bn1(self.conv1(x)))
        x = torch.relu(self.bn2(self.conv2(x)))
        x = torch.relu(self.bn3(self.conv3(x)))
        return self.fc(self.pool(x).squeeze(-1))

model = IMU1DCNN(24, 10, 25)
model.load_state_dict(torch.load(
    "D:/git_fork_sure_mine/MoveToPlay/output_cnn1d/cnn1d_model.pt",
    map_location="cpu", weights_only=True))
model.eval()

with open("D:/git_fork_sure_mine/MoveToPlay/output_cnn1d/norm_params.json") as f:
    norm = json.load(f)
with open("D:/git_fork_sure_mine/MoveToPlay/output_cnn1d/cnn1d_model_meta.json") as f:
    meta = json.load(f)

mean = np.array(norm["mean"], dtype=np.float32).reshape(1, 24, 1)
std = np.array(norm["std"], dtype=np.float32).reshape(1, 24, 1)
classes = meta["class_names"]

def predict(x_raw, label):
    x_norm = (x_raw - mean) / std
    with torch.no_grad():
        out = model(torch.FloatTensor(x_norm))
        probs = torch.softmax(out, dim=1)[0].numpy()
    pred_idx = probs.argmax()
    print(f"\n{label}:")
    print(f"  Predicted: {classes[pred_idx]} (conf={probs[pred_idx]:.3f})")
    top3 = np.argsort(probs)[::-1][:3]
    for i in top3:
        print(f"    {classes[i]:25s}: {probs[i]:.4f}")

# Test 1: all zeros (no gravity, no motion)
x1 = np.zeros((1, 24, 25), dtype=np.float32)
predict(x1, "Test 1: all zeros (unrealistic)")

# Test 2: typical idle with gravity on different axes per node
x2 = np.zeros((1, 24, 25), dtype=np.float32)
x2[0, 1, :] = 1.0   # node1 ay=1g
x2[0, 7, :] = -0.7  # node2 ay
x2[0, 13, :] = -1.0  # node3 ay
x2[0, 19, :] = 1.0  # node4 ay
predict(x2, "Test 2: static with gravity (idle-like)")

# Test 3: use actual training mean as input
x3 = np.tile(mean, (1, 1, 25))
predict(x3, "Test 3: input = training mean (most common state)")

# Print norm params for inspection
print("\n\nNormalization parameters:")
ch_names = []
for n in range(1, 5):
    for ch in ["ax", "ay", "az", "gx", "gy", "gz"]:
        ch_names.append(f"n{n}_{ch}")

print(f"{'Channel':<12} {'Mean':>10} {'Std':>10}")
for i, name in enumerate(ch_names):
    print(f"{name:<12} {norm['mean'][i]:>10.3f} {norm['std'][i]:>10.3f}")
