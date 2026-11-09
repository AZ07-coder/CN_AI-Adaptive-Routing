# Evaluation: baseline vs adaptive routing

Runs per mode: {'adaptive_ml': 24, 'adaptive_rule': 24, 'baseline': 24}

## UDP H1->H10 throughput (Mbps, higher=better)

```
mode           baseline  adaptive_rule  adaptive_ml
traffic_level                                      
low               2.611          2.634        2.634
medium            6.223          8.482        8.482
high              6.855          9.675        9.675
```

## UDP H1->H10 packet loss (%, lower=better)

```
mode           baseline  adaptive_rule  adaptive_ml
traffic_level                                      
low               1.030          0.364        0.364
medium           36.534         13.999       13.999
high             58.027         43.202       43.202
```

## UDP H1->H10 mean delay (ms, lower=better)

```
mode           baseline  adaptive_rule  adaptive_ml
traffic_level                                      
low              83.325         60.506       60.506
medium         1510.143        912.600      912.600
high           4734.196       3158.208     3158.208
```

## UDP H1->H10 jitter (ms, lower=better)

```
mode           baseline  adaptive_rule  adaptive_ml
traffic_level                                      
low               0.374          3.170        3.170
medium            1.277        369.632      369.632
high              1.259       1229.861     1229.861
```

## TCP H2->H9 throughput (Mbps, higher=better)

```
mode           baseline  adaptive_rule  adaptive_ml
traffic_level                                      
low               2.586          2.586        2.586
medium            4.925          4.925        4.925
high              4.925          4.925        4.925
```

## Cross-traffic loss (%, lower=better)

```
mode           baseline  adaptive_rule  adaptive_ml
traffic_level                                      
low               4.498          1.866        1.866
medium           12.023         10.700       10.700
high             13.187         19.091       19.091
```

## All flows: packets delivered (%, higher=better)

```
mode           baseline  adaptive_rule  adaptive_ml
traffic_level                                      
low              97.661         99.014       99.014
medium           85.268         92.520       92.520
high             69.720         75.266       75.266
```

ML decisions: 408, fallbacks to rule: 0
