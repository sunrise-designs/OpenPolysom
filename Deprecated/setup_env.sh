#!/bin/bash
cmake -B build -S .
cmake --build build --target python_venv

echo "--- Setup Complete ---"
echo "To activate your environment, run: source .venv/bin/activate"
