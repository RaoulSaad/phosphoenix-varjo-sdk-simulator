from abc import ABC, abstractmethod

import numpy as np
import torch
from dynaphos.simulator import GaussianSimulator

class BaseProcessingAlgorithm(ABC):
    "Defines an interface for all image processing algorithms."

    @abstractmethod
    def process(self, data: np.ndarray, params: dict, simulator: GaussianSimulator) -> torch.Tensor:
        raise NotImplementedError
