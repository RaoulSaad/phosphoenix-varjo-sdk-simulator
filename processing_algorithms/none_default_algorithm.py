from base_processing_algorithm import BaseProcessingAlgorithm

import cv2
import numpy as np
import torch
from dynaphos.simulator import GaussianSimulator

class NoneDefaultAlgorithm(BaseProcessingAlgorithm):
    """
    A image processing algorithm that applies no filter and generates phosphenes.
    """
    def process(self, data: np.ndarray, params: dict, simulator: GaussianSimulator) -> torch.Tensor:
        """
        Process the input data with no filter and generates phosphenes.
        Args: data (np.ndarray), params (dict), coordinates visual field (Map): Input image data, parameters and coordinates of the visual field.
        Returns: torch.Tensor: the phosphene image.
        """
        
        frame = data

        # Resize and preprocess the frame
        #frame = cv2.resize(frame, params['run']['resolution'])
        #frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        frame = cv2.GaussianBlur(frame, (3, 3), 0)
        
        # Process the image with the selected filter
        processed_img = frame

        stim_pattern = simulator.sample_stimulus(processed_img, rescale=True)
        
        return stim_pattern
