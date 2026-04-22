from base_processing_algorithm import BaseProcessingAlgorithm

import cv2
import numpy as np
import torch
from dynaphos.image_processing import canny_processor
from dynaphos.simulator import GaussianSimulator

THRESHOLD_HIGH = 100

class Canny100DefaultAlgorithm(BaseProcessingAlgorithm):
    """
    A image processing algorithm that applies the canny filter and generates phosphenes.
    """
    def process(self, data: np.ndarray, params: dict, simulator: GaussianSimulator):
        """
        Process the input data with the canny filter and generates phosphenes.
        Args: data (np.ndarray), params (dict), coordinates visual field (Map): Input image data, parameters and coordinates of the visual field.
        Returns: torch.Tensor: the phosphene image.
        """
        
        frame = data

        # Resize and preprocess the frame
        #frame = cv2.resize(frame, params['run']['resolution'])
        #frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        frame = cv2.GaussianBlur(frame, (3, 3), 0)
        
        # Process the image with the selected filter
        processed_img = None
        processed_img = canny_processor(frame, THRESHOLD_HIGH//2, THRESHOLD_HIGH)

        stim_pattern = simulator.sample_stimulus(processed_img, rescale=True)
        
        return stim_pattern
