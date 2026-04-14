import pickle
import numpy as np

# 1. Load the pickle file
with open('codebook.pkl', 'rb') as f:
    data = pickle.load(f)

# 2. Convert to a numpy array (if it isn't one already)
array_data = np.array(data)

# 3. Save as raw bytes to a .bin file
array_data.tofile('codebook.bin')
