#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <time.h>
#include <limits.h>
#include <stdbool.h>
#include <pthread.h>

#define MAX_DOCKS 30
#define MAX_CRANES 25
#define MAX_CARGO_COUNT 200
#define MAX_NEW_REQUESTS 100
#define MAX_AUTH_STRING_LEN 100
#define MAX_SHIPS 1100
#define MAX_SOLVERS 8

typedef struct ShipRequest {
    int shipId;
    int timestep;
    int category;
    int direction;
    int emergency;
    int waitingTime;
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
} ShipRequest;

typedef struct MainSharedMemory {
    char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
    ShipRequest newShipRequests[MAX_NEW_REQUESTS];
} MainSharedMemory;

typedef struct MessageStruct {
    long mtype;
    int timestep;
    int shipId;
    int direction;
    int dockId;
    int cargoId;
    int isFinished;
    union {
        int numShipRequests;
        int craneId;
    };
    } MessageStruct;

typedef struct SolverRequest {
    long mtype;
    int dockId;
    char authStringGuess[MAX_AUTH_STRING_LEN];
} SolverRequest;

typedef struct SolverResponse {
    long mtype;
    int guessIsCorrect;
} SolverResponse;

// Structure to store dock information
typedef struct {
    int id;
    int category;
    int crane_capacities[MAX_CRANES];
    int is_occupied;
    int occupying_ship_id;
    int occupying_direction;
    int dock_time;
    int last_cargo_time;
    int ship_category;
    int cranes_used[MAX_CRANES]; // Track which cranes are in use this timestep
} Dock;

// Structure to store ship information
typedef struct {
    int id;
    int direction;
    int category;
    int emergency;
    int arrival_time;
    int waiting_time;
    int num_cargo;
    int cargo[MAX_CARGO_COUNT];
    int cargo_moved[MAX_CARGO_COUNT]; // Track which cargo items have been moved
    int is_docked;
    int dock_assigned;
    int is_processed;
} Ship;

int shmKey,mmqKey,numSolvers,solverMsgq[8],numDocks,shmId,mmqId,solverMsgqId[8],timestep=1;
Dock docks[MAX_DOCKS];
Ship ships[MAX_SHIPS];
int numShips=0;
MainSharedMemory *mainMem;
char auth_chars[] = {'5', '6', '7', '8', '9', '.'};

// Function to generate the nth combination of the auth string
void generate_auth_string(int index, int length, char *auth_string) {
    int char_set_size;
    for (int i = length - 1; i >= 0; i--) {
        char_set_size = (i == 0 || i == length - 1) ? 5 : 6; // First/last positions cannot have '.'
        auth_string[i] = auth_chars[index % char_set_size];
        index /= char_set_size;
    }
    auth_string[length] = '\0';
}

// Optimized guessing function
bool optimized_guessing(int dockIndex, int authStringLength, int solverId, int numSolvers, int solverMsgqId[], MainSharedMemory *mainMem) {
    // Calculate total combinations
    int total_combinations = 1;
    for (int i = 0; i < authStringLength; i++) {
        total_combinations *= (i == 0 || i == authStringLength - 1) ? 5 : 6;
    }

    // Divide combinations among solvers
    int combinations_per_solver = (total_combinations + numSolvers - 1) / numSolvers; // Ceiling division
    int start_index = solverId * combinations_per_solver;
    int end_index = (solverId + 1) * combinations_per_solver - 1;
    if (end_index >= total_combinations) end_index = total_combinations - 1;

    SolverRequest solverRequest;
    SolverResponse solverResponse;

    solverRequest.mtype = 2;
    solverRequest.dockId = dockIndex;

    char auth_guess[MAX_AUTH_STRING_LEN];
    for (int combo = start_index; combo <= end_index; combo++) {
        // Generate auth string for this combination
        generate_auth_string(combo, authStringLength, auth_guess);
        strcpy(solverRequest.authStringGuess, auth_guess);

        // Send guess to solver
        if (msgsnd(solverMsgqId[solverId], &solverRequest, sizeof(solverRequest) - sizeof(solverRequest.mtype), 0) == -1) {
            printf("Error sending guess to solver %d\n", solverId);
            continue;
        }

        // Receive response from solver
        if (msgrcv(solverMsgqId[solverId], &solverResponse, sizeof(solverResponse) - sizeof(solverResponse.mtype), 3, 0) == -1) {
            perror("Error receiving message from solver");
            continue;
        }

        // Check if the guess is correct
        if (solverResponse.guessIsCorrect == 1) {
            strcpy(mainMem->authStrings[dockIndex], auth_guess);
            printf("Solver %d: Correct guess for Dock %d: %s\n", solverId, dockIndex, auth_guess);
            return true; // Stop further guessing
        }
    }

    return false; // No correct guess found in this range
}

// Structure to hold thread arguments
typedef struct {
    int solverId;
    int dockIndex;
    int authStringLength;
    int numSolvers;
    int *solverMsgqId;
    MainSharedMemory *mainMem;
    bool *isCorrect;
    pthread_mutex_t *mutex;
} ThreadArgs;

// Thread function for solver tasks
void* solver_thread(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    
    // Check if another thread already found the solution
    pthread_mutex_lock(args->mutex);
    bool alreadyCorrect = *(args->isCorrect);
    pthread_mutex_unlock(args->mutex);
    
    if(alreadyCorrect) 
        return NULL; // Skip if solution already found
    
    // Try to guess the auth string
    if(optimized_guessing(args->dockIndex, args->authStringLength, args->solverId, 
                          args->numSolvers, args->solverMsgqId, args->mainMem)) {
        // Set flag that solution was found
        pthread_mutex_lock(args->mutex);
        *(args->isCorrect) = true;
        pthread_mutex_unlock(args->mutex);
    }
    
    return NULL;
}

int main(int argc, char *argv[]){
    if(argc!=2){perror("Usage: ./scheduler <auth_string>"); exit(1);}
    int tc=atoi(argv[1]);
    char file[50];
    sprintf(file,"testcase%d/input.txt",tc);
    FILE *fp=fopen(file,"r");
    if(fp==NULL){perror("Error opening file"); exit(1);}
    fscanf(fp,"%d",&shmKey);
    fscanf(fp,"%d",&mmqKey);
    fscanf(fp,"%d",&numSolvers);
    for(int i=0;i<numSolvers;i++){
        fscanf(fp,"%d",&solverMsgq[i]);
    }
    fscanf(fp,"%d",&numDocks);
    for(int i=0;i<numDocks;i++){
        fscanf(fp,"%d",&docks[i].category);
        for(int j=0;j<docks[i].category;j++){
            fscanf(fp,"%d",&docks[i].crane_capacities[j]);
        }
    }
    fclose(fp);
    // printf("shmKey: %d\n",shmKey);
    // printf("mmqKey: %d\n",mmqKey);
    // printf("numSolvers: %d\n",numSolvers);
    // printf("solverMsgq: ");
    // for(int i=0;i<numSolvers;i++){
    //     printf("%d ",solverMsgq[i]);
    // }
    // printf("\n");
    // printf("numDocks: %d\n",numDocks);
    // for(int i=0;i<numDocks;i++){
    //     printf("Dock %d: ",i);
    //     printf("Category: %d ",docks[i].category);
    //     printf("Crane Capacities: ");
    //     for(int j=0;j<docks[i].category;j++){
    //         printf("%d ",docks[i].crane_capacities[j]);
    //     }
    //     printf("\n");
    // }
    
    shmId = shmget((key_t)shmKey, sizeof(MainSharedMemory), 0666);
    // printf("shmId: %d\n",shmId);
    // return 0;
    if (shmId == -1) {
        perror("Error creating shared memory");
        exit(1);
    }
    mainMem = (MainSharedMemory *)shmat(shmId, NULL, 0);
    if (mainMem == (MainSharedMemory *)-1) {
        perror("Error attaching shared memory");
        exit(1);
    }
    mmqId = msgget(mmqKey, 0666 | IPC_CREAT);
    if (mmqId == -1) {
        perror("Error creating message queue");
        exit(1);
    }
    for(int i=0;i<numSolvers;i++){
        solverMsgqId[i] = msgget(solverMsgq[i], 0666 | IPC_CREAT);
        if (solverMsgqId[i] == -1) {
            perror("Error creating solver message queue");
            exit(1);
        }
    }
    
    //Cleanup
    // if (shmdt(mainMem) == -1) {
    //     perror("Error detaching shared memory");
    // }
    // if (shmctl(shmId, IPC_RMID, NULL) == -1) {
    //     perror("Error removing shared memory");
    // }
    // if (msgctl(mmqId, IPC_RMID, NULL) == -1) {
    //     perror("Error removing message queue");
    // }
    // for(int i=0;i<numSolvers;i++){
    //     if (msgctl(solverMsgqId[i], IPC_RMID, NULL) == -1) {
    //         perror("Error removing solver message queue");
    //     }
    // }
    // return 0;

    // Initialize docks
    // for (int i = 0; i < numDocks; i++) {
    //     docks[i].id = i;
    //     docks[i].is_occupied = 0;
    //     docks[i].occupying_ship_id = -1;
    //     docks[i].occupying_direction = -1;
    //     docks[i].dock_time = 0;
    //     docks[i].last_cargo_time = 0;
    //     docks[i].ship_category = -1;
    //     for (int j = 0; j < MAX_CRANES; j++) {
    //         docks[i].cranes_used[j] = 0;
    //     }
    // }
    // Initialize ships
    for (int i = 0; i < MAX_SHIPS; i++) {
        ships[i].id = -1;
        ships[i].direction = 0;
        ships[i].category = -1;
        ships[i].emergency = 0;
        ships[i].arrival_time = -1;
        ships[i].waiting_time = -1;
        ships[i].num_cargo = 0;
        for (int j = 0; j < MAX_CARGO_COUNT; j++) {
            ships[i].cargo[j] = -1;
            ships[i].cargo_moved[j] = 0;
        }
        ships[i].is_docked = 0;
        ships[i].dock_assigned = -1;
        ships[i].is_processed = 0;
    }
    bool isFinished = false;
    while(!isFinished){
        printf("Timestep %d\n",timestep);
        MessageStruct msg;
        if(msgrcv(mmqId, &msg, sizeof(msg) - sizeof(msg.mtype), 1, 0) == -1){
            perror("Error receiving message");
            exit(1);
        }
        printf("No of new ship requests is %d\n",msg.numShipRequests);
        if(msg.isFinished){
            isFinished = true;
            printf("Scheduler finished\n");
            break;
        }
        int numShipRequests = msg.numShipRequests;
        for(int i=0;i<numShipRequests;i++){
            ShipRequest shipRequest = mainMem->newShipRequests[i];
            // printf("Ship %d: Direction %d, Category %d, Emergency %d, Arrival Time %d, Waiting Time %d, Num Cargo %d\n",
            //         shipRequest.shipId, shipRequest.direction, shipRequest.category, shipRequest.emergency, shipRequest.timestep, shipRequest.waitingTime, shipRequest.numCargo);
            for(int j=0;j<MAX_SHIPS;++j){
                if(ships[j].id !=-1)continue;
                ships[j].id = shipRequest.shipId;
                ships[j].direction = shipRequest.direction;
                ships[j].category = shipRequest.category;
                ships[j].emergency = shipRequest.emergency;
                ships[j].arrival_time = timestep;
                ships[j].waiting_time = shipRequest.waitingTime;
                ships[j].num_cargo = shipRequest.numCargo;
                for(int k=0;k<shipRequest.numCargo;k++){
                    ships[j].cargo[k] = shipRequest.cargo[k];
                    ships[j].cargo_moved[k] = 0;
                }
                ships[j].is_docked = 0;
                ships[j].dock_assigned = -1;
                ships[j].is_processed = 0;
                if(ships[j].id != -1)printf("Ship %d: Direction %d, Category %d, Emergency %d, Arrival Time %d, Waiting Time %d, Num Cargo %d\n",
                        ships[j].id, ships[j].direction, ships[j].category, ships[j].emergency, ships[j].arrival_time, ships[j].waiting_time, ships[j].num_cargo);
                break;
            }                             
        }
        int free_dock_indices[MAX_DOCKS];
        int free_dock_count = 0;
        for(int i = 0; i < numDocks; i++) {
            if (docks[i].is_occupied == 0) {
                free_dock_indices[free_dock_count++] = i;
            }
        }
        // Sort free docks by category in ascending order
        for (int i = 0; i < free_dock_count - 1; i++) {
            for (int j = 0; j < free_dock_count - i - 1; j++) {
                if (docks[free_dock_indices[j]].category >= docks[free_dock_indices[j + 1]].category) {
                    int temp = free_dock_indices[j];
                    free_dock_indices[j] = free_dock_indices[j + 1];
                    free_dock_indices[j + 1] = temp;
                }
            }
        }
        int emergency_ship_indices[MAX_SHIPS];
        int emergency_count = 0;
        for(int i = 0; i < MAX_SHIPS; i++) {
            if (ships[i].id != -1 && ships[i].emergency == 1) {
                emergency_ship_indices[emergency_count++] = i;
            }
        }
        // Sort emergency ships by category in descending order
        for (int i = 0; i < emergency_count - 1; i++) {
            for (int j = 0; j < emergency_count - i - 1; j++) {
                if (ships[emergency_ship_indices[j]].category <= ships[emergency_ship_indices[j + 1]].category) {
                    int temp = emergency_ship_indices[j];
                    emergency_ship_indices[j] = emergency_ship_indices[j + 1];
                    emergency_ship_indices[j + 1] = temp;
                }
            }
        }
        for(int i=0;i<emergency_count;i++){
            int shipIndex = emergency_ship_indices[i];
            if(ships[shipIndex].is_docked)continue;
            int dockIndex = -1;
            for(int j=0;j<free_dock_count;j++){
                if(docks[free_dock_indices[j]].category >= ships[shipIndex].category && docks[free_dock_indices[j]].is_occupied == 0){
                    dockIndex = free_dock_indices[j];
                    break;
                }
            }
            if(dockIndex == -1)continue;
            docks[dockIndex].is_occupied = 1;
            docks[dockIndex].occupying_ship_id = ships[shipIndex].id;
            docks[dockIndex].occupying_direction = ships[shipIndex].direction;
            docks[dockIndex].dock_time = timestep;
            docks[dockIndex].last_cargo_time = timestep;
            docks[dockIndex].ship_category = ships[shipIndex].category;
            ships[shipIndex].is_docked = 1;
            ships[shipIndex].dock_assigned = dockIndex;

            printf("Emergency Ship %d docked at Dock %d\n", ships[shipIndex].id, dockIndex);
            MessageStruct dockMsg;
            dockMsg.mtype = 2;
            dockMsg.dockId = dockIndex;
            dockMsg.shipId = ships[shipIndex].id;
            dockMsg.direction = ships[shipIndex].direction;

            if(msgsnd(mmqId, &dockMsg, sizeof(dockMsg) - sizeof(dockMsg.mtype), 0) == -1){
                printf("Error sending dock message for emergency ship %d", ships[shipIndex].id);
                exit(1);
            }
        }
        int normal_ship_indices[MAX_SHIPS];
        int normal_count = 0;
        for(int i = 0; i < MAX_SHIPS; i++) {
            if (ships[i].id != -1 && ships[i].emergency == 0) {
                normal_ship_indices[normal_count++] = i;
            }
        }
        // Sort normal ships by category in descending order
        for (int i = 0; i < normal_count - 1; i++) {
            for (int j = 0; j < normal_count - i - 1; j++) {
                if (ships[normal_ship_indices[j]].category < ships[normal_ship_indices[j + 1]].category) {
                    int temp = normal_ship_indices[j];
                    normal_ship_indices[j] = normal_ship_indices[j + 1];
                    normal_ship_indices[j + 1] = temp;
                }
            }
        }
        for(int i=0;i<normal_count;i++){
            int shipIndex = normal_ship_indices[i];
            if(ships[shipIndex].is_docked)continue;
            int dockIndex = -1;
            for(int j=0;j<free_dock_count;j++){
                if(docks[free_dock_indices[j]].category >= ships[shipIndex].category && docks[free_dock_indices[j]].is_occupied == 0){
                    dockIndex = free_dock_indices[j];
                    break;
                }
            }
            if(dockIndex == -1)continue;
            docks[dockIndex].is_occupied = 1;
            docks[dockIndex].occupying_ship_id = ships[shipIndex].id;
            docks[dockIndex].occupying_direction = ships[shipIndex].direction;
            docks[dockIndex].dock_time = timestep;
            docks[dockIndex].last_cargo_time = timestep;
            docks[dockIndex].ship_category = ships[shipIndex].category;
            ships[shipIndex].is_docked = 1;
            ships[shipIndex].dock_assigned = dockIndex;

            printf("Normal Ship %d docked at Dock %d\n", ships[shipIndex].id, dockIndex);
            MessageStruct dockMsg;
            dockMsg.mtype = 2;
            dockMsg.dockId = dockIndex;
            dockMsg.shipId = ships[shipIndex].id;
            dockMsg.direction = ships[shipIndex].direction;

            if(msgsnd(mmqId, &dockMsg, sizeof(dockMsg) - sizeof(dockMsg.mtype), 0) == -1){
                printf("Error sending dock message for normal ship %d", ships[shipIndex].id);
                exit(1);
            }
        }
        // for(int i=0;i<free_dock_count;i++){
        //     int dockIndex = free_dock_indices[i];
        //     if(docks[dockIndex].is_occupied == 1)continue;
        //     for(int j=0;j<MAX_SHIPS;j++){
        //         if(ships[j].id == -1 || ships[j].is_docked || docks[dockIndex].is_occupied == 1)continue;
        //         if(ships[j].category > docks[dockIndex].category)continue;
        //         docks[dockIndex].is_occupied = 1;
        //         docks[dockIndex].occupying_ship_id = ships[j].id;
        //         docks[dockIndex].occupying_direction = ships[j].direction;
        //         docks[dockIndex].dock_time = timestep;
        //         docks[dockIndex].last_cargo_time = timestep;
        //         docks[dockIndex].ship_category = ships[j].category;
        //         ships[j].is_docked = 1;
        //         ships[j].dock_assigned = dockIndex;

        //         printf("Ship %d with direction %d docked at Dock %d\n", ships[j].id, ships[j].direction, dockIndex);
        //         MessageStruct dockMsg;
        //         dockMsg.mtype = 2;
        //         dockMsg.dockId = dockIndex;
        //         dockMsg.shipId = ships[j].id;
        //         dockMsg.direction = ships[j].direction;

        //         if(msgsnd(mmqId, &dockMsg, sizeof(dockMsg) - sizeof(dockMsg.mtype), 0) == -1){
        //             printf("Error sending dock message for ship %d with direction %d", ships[j].id, ships[j].direction);
        //             exit(1);
        //         }
        //     }
        // }
        for(int i=0;i<MAX_SHIPS;i++){
            if(ships[i].id == -1)continue;
            if(ships[i].is_docked == 0)continue;
            int dockIndex = ships[i].dock_assigned;
            if(docks[dockIndex].dock_time == timestep)continue;
            // Sort cargo indices according to quantity
            int cargo_indices[ships[i].num_cargo];
            for(int j=0;j<ships[i].num_cargo;j++){
                cargo_indices[j] = j;
            }
            for(int j=0;j<ships[i].num_cargo-1;j++){
                for(int k=0;k<ships[i].num_cargo-j-1;k++){
                    if(ships[i].cargo[cargo_indices[k]] < ships[i].cargo[cargo_indices[k+1]]){
                        int temp = cargo_indices[k];
                        cargo_indices[k] = cargo_indices[k+1];
                        cargo_indices[k+1] = temp;
                    }
                }
            }
            // Reset cranes used for this timestep
            for(int j=0;j<docks[dockIndex].category;j++){
                docks[dockIndex].cranes_used[j] = 0;
            }
            for(int j=0;j<ships[i].num_cargo;j++){
                int cargoId = cargo_indices[j];
                if(ships[i].cargo_moved[cargoId] == 1)continue;
                for(int k=0;k<docks[dockIndex].category;k++){
                    if(docks[dockIndex].cranes_used[k] == 0 && docks[dockIndex].crane_capacities[k] >= ships[i].cargo[cargoId]){
                        docks[dockIndex].cranes_used[k] = 1;
                        docks[dockIndex].last_cargo_time = timestep;
                        ships[i].cargo_moved[cargoId] = 1;
                        MessageStruct craneMsg;
                        craneMsg.mtype = 4;
                        craneMsg.timestep = timestep;
                        craneMsg.shipId = ships[i].id;
                        craneMsg.direction = ships[i].direction;
                        craneMsg.craneId = k;
                        craneMsg.dockId = dockIndex;
                        craneMsg.cargoId = cargoId;

                        if(msgsnd(mmqId, &craneMsg, sizeof(craneMsg) - sizeof(craneMsg.mtype), 0) == -1){
                            printf("Error sending crane message for ship %d and direction %d", ships[i].id, ships[i].direction);
                            exit(1);
                        }
                        printf("Ship %d with direction %d moved cargo %d using crane %d at Dock %d\n", ships[i].id, ships[i].direction, cargoId, k, dockIndex);
                        break;
                    }
                }
            }
            bool allMoved = true;
            for(int k=0;k<ships[i].num_cargo;k++){
                if(ships[i].cargo_moved[k] == 0){
                    allMoved = false;
                    break;
                }
            }
            if(allMoved){
                printf("All cargo moved for ship %d with direction %d at timestep %d\n", ships[i].id, ships[i].direction, timestep);
                if(docks[dockIndex].last_cargo_time == timestep)continue;
                // Calculate auth string length 
                int authStringLength = docks[dockIndex].last_cargo_time - docks[dockIndex].dock_time;
                printf("Auth string length is %d\n", authStringLength);

                // Send initial request to all solvers
                for(int s=0; s<numSolvers; s++){
                    SolverRequest solverRequest;
                    solverRequest.mtype = 1;
                    solverRequest.dockId = dockIndex;
                    if(msgsnd(solverMsgqId[s], &solverRequest, sizeof(solverRequest) - sizeof(solverRequest.mtype), 0) == -1){
                        printf("Error sending initial message to solver %d\n", s);
                    }
                }

                // Use multi-threaded approach with all solvers working in parallel
                bool isCorrect = false;
                pthread_t threads[MAX_SOLVERS];
                ThreadArgs threadArgs[MAX_SOLVERS];
                pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

                for(int s=0; s<numSolvers; s++) {
                    threadArgs[s].solverId = s;
                    threadArgs[s].dockIndex = dockIndex;
                    threadArgs[s].authStringLength = authStringLength;
                    threadArgs[s].numSolvers = numSolvers;
                    threadArgs[s].solverMsgqId = solverMsgqId;
                    threadArgs[s].mainMem = mainMem;
                    threadArgs[s].isCorrect = &isCorrect;
                    threadArgs[s].mutex = &mutex;
                    pthread_create(&threads[s], NULL, solver_thread, (void *)&threadArgs[s]);
                }

                for(int s=0; s<numSolvers; s++) {
                    pthread_join(threads[s], NULL);
                }

                // After finding the correct auth string, proceed with undocking
                MessageStruct undockMsg;
                undockMsg.mtype = 3;
                undockMsg.timestep = timestep;
                undockMsg.shipId = ships[i].id;
                undockMsg.direction = ships[i].direction;
                undockMsg.dockId = dockIndex;

                if(msgsnd(mmqId, &undockMsg, sizeof(undockMsg) - sizeof(undockMsg.mtype), 0) == -1){
                    printf("Error sending undock message for ship %d and direction %d", ships[i].id, ships[i].direction);
                    exit(1);
                }
                docks[dockIndex].is_occupied = 0;
                docks[dockIndex].occupying_ship_id = -1;
                ships[i].id = -1;
            }
        }
        // Clear unattended ships
        for(int j=0;j<MAX_SHIPS;j++){
            if(ships[j].id !=-1 && ships[j].direction == 1 && (ships[j].arrival_time + ships[j].waiting_time <= timestep) && ships[j].is_docked == 0 && ships[j].emergency == 0){
                ships[j].id = -1;
            }
        }
        MessageStruct timestepMsg;
        timestepMsg.mtype = 5;
        if(msgsnd(mmqId, &timestepMsg, sizeof(timestepMsg) - sizeof(timestepMsg.mtype), 0) == -1){
            printf("Error sending timestep message");
            exit(1);
        }
        timestep++;
    }
    // Cleanup
    // sleep(1);
    // if (shmdt(mainMem) == -1) {
    //     perror("Error detaching shared memory");
    //     exit(1);
    // }
    // if (shmctl(shmId, IPC_RMID, NULL) == -1) {
    //     perror("Error removing shared memory");
    //     exit(1);
    // }
    // if (msgctl(mmqId, IPC_RMID, NULL) == -1) {
    //     perror("Error removing message queue");
    //     exit(1);
    // }
    // for(int i=0;i<numSolvers;i++){
    //     if (msgctl(solverMsgqId[i], IPC_RMID, NULL) == -1) {
    //         perror("Error removing solver message queue");
    //         exit(1);
    //     }
    // }
    return 0;
}
