# Synthetic Camera-Cut Fixture

Frames 1-4: stationary camera at origin (identity matrix_world).
Frame 5: 10-unit jump in X (position=[10,0,0]).
Used by Plan dlss-pipeline-fix Task 3 (camera cut detection tests).
Default translation threshold is 5.0 → frame 5 always triggers a cut.
