#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <algorithm>
#include <sstream>
#include <set>
#include <pthread.h>

using namespace std;

// struct pentru datele partajate intre thread-uri
struct SharedData {
    pthread_mutex_t file_mutex; // mutex pentru a proteja accesul la urmatorul fisier
    pthread_mutex_t results_mutex; // mutex pentru a proteja accesul la word_locations
    pthread_barrier_t barrier; // bariera pentru a sincroniza thread-urile
    vector<string> input_files; // numele fisierelor de intrare
    int next_file_index = 0; // indexul urmatorului fisier de procesat
    map<string, set<int>> word_locations; // rezultatele partiale
    int num_reducers; // numarul de thread-uri reducer
};

// struct pentru argumentele thread-ului
struct ThreadArgs {
    int id; // id-ul thread-ului
    SharedData *shared_data; // datele partajate
};

// functie pentru procesarea unui fisier
void read_input_files(const string& filename, SharedData& data) {
    // deschid fisierul si citesc numarul de fisiere
    ifstream fin(filename);
    int num_files;
    fin >> num_files;
    string file;
    for (int i = 0; i < num_files; i++) {
        fin >> file;
        // adaug numele fisierului in vectorul de fisiere
        data.input_files.push_back(file);
    }
    fin.close();
}

// functie pentru procesarea unui fisier - mapper
void process_file(int file_id, const string& filename, SharedData& data) {
    ifstream fin(filename);
    // map pentru a retine cuvintele si pozitiile lor in fisier
    map<string, set<int>> local_words; 
    string word; 
    word.reserve(100); // rezerv spatiu pt eficienta
    while (fin >> word) {
        // fac toate cuvintele cu litere mici
        for (char& c : word) {
            c = tolower(c);
        }
        // elimin caracterele care nu sunt litere
        string word_only_characters;
        for (char c : word) { 
            // daca este litera, o adaug in cuvant
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                word_only_characters += c;
            }
        }
        // adaug cuvantul in rezultate
        if (!word_only_characters.empty()) {
            // adaug pozitia cuvantului in fisier
            local_words[word_only_characters].insert(file_id);
        }
    }
    fin.close();

    // lock la mutex-ul de rezultate pt a scrie cuvintele
    pthread_mutex_lock(&data.results_mutex);
    // adaug cuvintele in rezultate
    for (const auto& entry : local_words) {
        data.word_locations[entry.first].insert(entry.second.begin(), entry.second.end());
    }
    // eliberez mutex-ul
    pthread_mutex_unlock(&data.results_mutex);
}

// functie pentru thread-ul mapper
void* mapper(void* arg) {
    // extrag argumentele thread-ului
    ThreadArgs* args = (ThreadArgs*)arg;
    SharedData* data = args->shared_data;
    // cat timp mai sunt fisiere de procesat
    while (true) {
        // extrag urmatorul fisier de procesat
        pthread_mutex_lock(&data->file_mutex);
        if (data->next_file_index >= data->input_files.size()) {
            pthread_mutex_unlock(&data->file_mutex);
            break;
        }
        // incrementez indexul urmatorului fisier
        int file_id = data->next_file_index + 1;
        // extrag numele fisierului
        string filename = data->input_files[data->next_file_index++];
        // eliberez mutex-ul
        pthread_mutex_unlock(&data->file_mutex);
        // procesez fisierul
        process_file(file_id, filename, *data);
    }
    // astept la bariera
    pthread_barrier_wait(&data->barrier);
    return NULL;
}

// functie pentru thread-ul reducer
void* reducer(void* arg) {
    // extrag argumentele thread-ului
    ThreadArgs* args = (ThreadArgs*)arg;
    SharedData* data = args->shared_data;

    // astept la bariera
    pthread_barrier_wait(&data->barrier);
    // calculez intervalul de cuvinte pe care il procesez
    char start_letter = 'a' + ((26 * args->id) / data->num_reducers);
    char end_letter = 'a' + ((26 * (args->id + 1)) / data->num_reducers) - 1;
    // daca sunt la ultimul reducer, procesez si literele de la 'z'
    if (args->id == data->num_reducers - 1) {
        end_letter = 'z';
    }
    
    // procesez cuvintele din intervalul respectiv
    map<string, set<int>> words_for_letters;
    // lock la mutex-ul de rezultate pentru a citi datele
    pthread_mutex_lock(&data->results_mutex);
    for (const auto& entry : data->word_locations) {
        char first_letter = entry.first[0];
        if (first_letter >= start_letter && first_letter <= end_letter) {
            words_for_letters[entry.first] = entry.second;
        }
    }
    // eliberez mutex-ul
    pthread_mutex_unlock(&data->results_mutex);

    // procesez cuvintele in ordine alfabetica
    // pentru fiecare litera creez un fisier
    for (char c = start_letter; c <= end_letter; c++) {
        // creez numele fisierului
        string filename(1,c); // numele fisierului este litera
        // adaug extensia .txt
        filename += ".txt";
        // folosim ios::app pentru a scrie la finalul fisierului
        ofstream fout(filename, ios::app);
    
        // declar un vector de perechi pentru a sorta cuvintele
        vector<pair<string, set<int>>> words_list; 
        // adaug cuvintele in vector
        for (const auto& elem : words_for_letters) {
            if (elem.first[0] == c) {
                words_list.push_back(elem);
            }
        }

        // sortez dupa numarul de aparitii si alfabetic
        sort(words_list.begin(), words_list.end(),[](const auto& a, const auto& b) {
            if(a.second.size() != b.second.size())
                return a.second.size() > b.second.size();
            return a.first < b.first;
            });

        // folosesc "output" pentru scriere
        stringstream output;
        for (const auto& word : words_list) {
            // formatul de scriere in fisier
            output << word.first << ":[";
            bool first = true;
            // scriu id-urile fisierelor
            for (int id : word.second) {
                if (!first) {
                    output << " ";
                }
                output << id;
                first = false;
            }
            output << "]\n";
        }
        fout << output.str();
        fout.close();
    }
    return NULL;    
}


int main(int argc, char **argv) {
   int num_mappers = atoi(argv[1]);
   int num_reducers = atoi(argv[2]);
   string input_file = argv[3];
   
   SharedData shared_data;
   shared_data.num_reducers = num_reducers;
   
   // calculam numarul total de thread-uri
   int num_total = num_mappers + num_reducers;

   pthread_mutex_init(&shared_data.file_mutex, NULL);
   pthread_mutex_init(&shared_data.results_mutex, NULL);
   pthread_barrier_init(&shared_data.barrier, NULL, num_total);

   read_input_files(input_file, shared_data);

   // vector pentru ambele tipuri de thread-uri
   pthread_t both_threads[num_total];
   ThreadArgs mapper_data[num_mappers];
   ThreadArgs reducer_data[num_reducers];

   // toate thread-urile pe o iteratie de for (ca sa respectam cerinta)
   for (int i = 0; i < num_total; ++i) {
       if (i < num_mappers) {
           // thread-uri mapper
           mapper_data[i] = {i, &shared_data};
           pthread_create(&both_threads[i], nullptr, mapper, &mapper_data[i]);
       } else {
           // thread-uri reducer
           int reducer_index = i - num_mappers;
           reducer_data[reducer_index] = {reducer_index, &shared_data};
           pthread_create(&both_threads[i], nullptr, reducer, &reducer_data[reducer_index]);
       }
   }

   // join la toate thread-urile dintr-un singur for
   for (int i = 0; i < num_total; ++i) {
       pthread_join(both_threads[i], nullptr);
   }

   pthread_mutex_destroy(&shared_data.file_mutex);
   pthread_mutex_destroy(&shared_data.results_mutex);
   pthread_barrier_destroy(&shared_data.barrier);

   return 0;
}