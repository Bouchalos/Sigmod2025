Για να τρέξουμε το πρόγραμμα κάνουμε: 
cmake --build build --target fast_unc -- -j 6
./build/fast_unc   plans.json

Και για τα τεστ τρέχουμε 
cmake --build build --target test_unchained test_column_store
./build/test_column_store
./build/test_unchained
