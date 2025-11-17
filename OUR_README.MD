sdi2200113: Ανδρέας Μπουχαλάκης
sdi2200182: Δημήτρης Τζάκας

Μπουχαλάκης: Hopscotch - unit test, yaml
Τζάκας: cuckoo - unit test, macros
από κοινού: robinhood - unit test

Για την υλοποίηση των τριών hashtable μοιράσαμε τις δουλειές ως με τον παραπάνω τρόπο, από κοινού έγινε το robinhood το οποίο 
υλοποιήσαμε μέσω share screen. 
Δημήτρης: Ασχολήθηκα με την υλοποίηση του Cuckoo Hashing.
Πιο συγκεκριμένα:
Έφτιαξα την κλάση CuckooMultiMap στο αρχείο CuckooHash.h, που υλοποιεί τον αλγόριθμο cuckoo hashing με δύο πίνακες και δύο 
συναρτήσεις κατακερματισμού.

Ενσωμάτωσα τον αλγόριθμο στο κοινό interface της εφαρμογής μέσω του αρχείου macro.h, επιτρέποντας επιλογή μεταξύ των υλοποιήσεων
(Robin Hood, Hopscotch, Cuckoo, std::unordered_map).

Έγραψα το αρχείο test_cuckoo.cpp με unit tests χρησιμοποιώντας Catch2, για να ελέγχω τη σωστή λειτουργία του αλγορίθμου 
(εισαγωγές, αναζητήσεις, rehash, πολλαπλές τιμές στο ίδιο κλειδί κλπ).

Έκανα δοκιμές για την απόδοση και τη σταθερότητα της δομής.

Ανδρέας: Υλοποίησα το hopscotch, ένα hashtable που η εισαγωγή του βασίζεται σε γειτονιές και κάνει rehash με τέτοιο τρόπο 
ώστε να φροντίζει η αναζήτηση/εισαγωγή να είναι σε χρόνο O(1), αυτό επιτυγχάνεται προσθέτοντας τον όρο της γειτονιάς ο οποίος σου λέει
αν πολλά keys hasharoun στο ίδιο bucket να κάνει rehash, η hash function που επέλεξα είναι η hash με modulo το μέγεθος του table ενώ 
η υλοποίηση χρισημοποιεί και το bitmap. 

Για το .yaml πήρα ιδέες και το εφάρμοσα ανάλογα με το παραπάνω repository: https://github.com/chatziko-k08/lecture-code

Ενώ υλοποίησα και κάποια απλά unit tests για το hopscotch έτσι ώστε να ελέγχω ότι λειτουργεί ορθά και αποτελεσματικά. Αυτά που τεστάρω
είναι η εισαγωγή το rehash η find καθώς και η συνάρτηση του iterator. 

Από κοινού: Φτιάξαμε το robinhood και τα unit tests του
Για την υλοποίηση του robinhood αρχικά είχαμε χρισημοποιήσει για hash function την hash η οποία μας γυρνούσε πολύ κακά αποτελέσματα στο χρόνο
για αυτό χρησιμοποιήσαμε την DJB2 η οποία μας έφερε και πολύ καλά αποτελέσματα. 

Ενώ φτιάξαμε και unit tests για κάθε συνάρτηση του αλγορίθμου.

---------------------------------------------------------------------------------------------------------------------------------------------

Report: Μόνο από την υλοποίηση των 3 διαφορετικών hash table δεν είδαμε κάποια σημαντική διαφορά στον χρόνο, υπήρξε βελτίωση αλλά μέχρι εκεί
Έπειτα από πέντε φορές που τρέξαμε κάθε αλγόριθμο, βγάλαμε τα εξής αποτελέσματα: 
Cuckoo μ.ο. 183120 ms
Hopscotch μ.ο. 180205 ms
RobinHood μ.ο. 177144 ms
Unordered_map μ.ο. 190469 ms

Ο τρόπος με τον οποίο τρέχουμε τα προγράμματα είναι: 
cmake -S . -B build
Δημιουργεί μια execute με καθένα από αυτά τα hashtables
cmake --build build --target fast_robin fast_hop fast_cuckoo fast_std -- -j 6



Για να τρέξουμε τα queries:
./build/fast_robin   plans.json
./build/fast_hop     plans.json
./build/fast_cuckoo  plans.json
./build/fast_std     plans.json

my tests: Για να φτιάξουμε τα test
cmake --build build --target test_robinhood
cmake --build build --target test_cuckoo
cmake --build build --target test_hopscotch

Για να τρέξουμε τα tests
./build/test_cuckoo 
./build/test_robinhood 
./build/test_hopscotch

#demo pull request