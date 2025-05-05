document.addEventListener('DOMContentLoaded', function() {
    // Éléments DOM principaux
    const streamsList = document.getElementById('streamsList');
    const alertsList = document.getElementById('alertsList');
    const systemStatus = document.getElementById('systemStatus');
    const streamForm = document.getElementById('streamForm');
    const saveStreamBtn = document.getElementById('saveStreamBtn');
    const addStreamModal = new bootstrap.Modal(document.getElementById('addStreamModal'));
    const streamDetailsModal = new bootstrap.Modal(document.getElementById('streamDetailsModal'));
    const streamDetails = document.getElementById('streamDetails');

    // Initialisation
    loadStreams();
    loadAlerts();
    loadSystemStatus();

    // Actualisation périodique
    setInterval(loadStreams, 5000);
    setInterval(loadAlerts, 5000);
    setInterval(loadSystemStatus, 5000);

    // Écouteurs d'événements
    saveStreamBtn.addEventListener('click', saveStream);

    // Fonction pour charger la liste des flux
    function loadStreams() {
        fetch('/api/streams')
            .then(response => response.json())
            .then(streams => {
                streamsList.innerHTML = '';
                streams.forEach(stream => {
                    const streamItem = document.createElement('div');
                    streamItem.className = 'list-group-item stream-item';
                    
                    const statusClass = stream.running ? 'bg-success' : 'bg-secondary';
                    const statusText = stream.running ? 'En cours' : 'Arrêté';
                    
                    streamItem.innerHTML = `
                        <div>
                            <h5>${stream.name}</h5>
                            <small>${stream.hlsInput}</small>
                        </div>
                        <div class="d-flex align-items-center">
                            <span class="badge ${statusClass} status-badge me-2">${statusText}</span>
                            <div class="stream-controls">
                                <button class="btn btn-sm btn-info view-details" data-id="${stream.id}"><i class="bi bi-info-circle"></i> Détails</button>
                                ${!stream.running ? 
                                    `<button class="btn btn-sm btn-success start-stream" data-id="${stream.id}"><i class="bi bi-play-fill"></i> Démarrer</button>` : 
                                    `<button class="btn btn-sm btn-danger stop-stream" data-id="${stream.id}"><i class="bi bi-stop-fill"></i> Arrêter</button>`
                                }
                                <button class="btn btn-sm btn-primary edit-stream" data-id="${stream.id}"><i class="bi bi-pencil"></i> Éditer</button>
                                <button class="btn btn-sm btn-danger delete-stream" data-id="${stream.id}"><i class="bi bi-trash"></i> Supprimer</button>
                            </div>
                        </div>
                    `;
                    
                    streamsList.appendChild(streamItem);
                });
                
                // Ajouter les écouteurs d'événements pour les boutons
                document.querySelectorAll('.start-stream').forEach(btn => {
                    btn.addEventListener('click', startStream);
                });
                
                document.querySelectorAll('.stop-stream').forEach(btn => {
                    btn.addEventListener('click', stopStream);
                });
                
                document.querySelectorAll('.edit-stream').forEach(btn => {
                    btn.addEventListener('click', editStream);
                });
                
                document.querySelectorAll('.delete-stream').forEach(btn => {
                    btn.addEventListener('click', deleteStream);
                });
                
                document.querySelectorAll('.view-details').forEach(btn => {
                    btn.addEventListener('click', viewStreamDetails);
                });
            })
            .catch(error => {
                console.error('Erreur lors du chargement des flux:', error);
            });
    }

    // Fonction pour charger les alertes
    function loadAlerts() {
        fetch('/api/alerts')
            .then(response => response.json())
            .then(alerts => {
                alertsList.innerHTML = '';
                alerts.slice(0, 10).forEach(alert => {
                    const alertItem = document.createElement('div');
                    alertItem.className = `list-group-item alert-item alert-${alert.level.toLowerCase()}`;
                    
                    alertItem.innerHTML = `
                        <div class="d-flex justify-content-between">
                            <strong>${alert.source}</strong>
                            <span class="badge bg-${getLevelClass(alert.level)}">${alert.level}</span>
                        </div>
                        <p class="mb-1">${alert.message}</p>
                        <small class="timestamp">${alert.timestamp}</small>
                    `;
                    
                    alertsList.appendChild(alertItem);
                });
            })
            .catch(error => {
                console.error('Erreur lors du chargement des alertes:', error);
            });
    }

    // Fonction pour charger l'état du système
    function loadSystemStatus() {
        fetch('/api/system')
            .then(response => response.json())
            .then(status => {
                systemStatus.innerHTML = `
                    <div class="system-status-item">
                        <span>Flux totaux:</span>
                        <strong>${status.totalStreams}</strong>
                    </div>
                    <div class="system-status-item">
                        <span>Flux actifs:</span>
                        <strong>${status.runningStreams}</strong>
                    </div>
                    <div class="system-status-item">
                        <span>CPU:</span>
                        <div class="w-50">
                            <div class="progress">
                                <div class="progress-bar" role="progressbar" style="width: ${status.cpuUsage}%"></div>
                            </div>
                            <small>${status.cpuUsage.toFixed(1)}%</small>
                        </div>
                    </div>
                    <div class="system-status-item">
                        <span>Mémoire:</span>
                        <div class="w-50">
                            <div class="progress">
                                <div class="progress-bar" role="progressbar" style="width: ${status.memoryUsage}%"></div>
                            </div>
                            <small>${status.memoryUsage.toFixed(1)}%</small>
                        </div>
                    </div>
                    <div class="system-status-item">
                        <span>Uptime:</span>
                        <strong>${status.uptime}</strong>
                    </div>
                    <div class="system-status-item">
                        <span>Version:</span>
                        <strong>${status.version}</strong>
                    </div>
                `;
            })
            .catch(error => {
                console.error('Erreur lors du chargement de l\'état du système:', error);
            });
    }

    // Fonction pour enregistrer un flux
    function saveStream() {
        const formData = new FormData(streamForm);
        const streamData = {};
        
        formData.forEach((value, key) => {
            if (key === 'multicastPort' || key === 'bufferSize') {
                streamData[key] = parseInt(value);
            } else if (key === 'autoStart') {
                streamData[key] = true;
            } else {
                streamData[key] = value;
            }
        });
        
        // Si l'ID est vide, c'est un nouveau flux
        if (!streamData.id) {
            delete streamData.id;
        }
        
        fetch('/api/streams', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(streamData)
        })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                addStreamModal.hide();
                streamForm.reset();
                loadStreams();
            } else {
                alert('Erreur: ' + data.error);
            }
        })
        .catch(error => {
            console.error('Erreur lors de l\'enregistrement du flux:', error);
            alert('Erreur lors de l\'enregistrement du flux');
        });
    }

    // Fonction pour démarrer un flux
    function startStream(event) {
        const streamId = event.target.getAttribute('data-id');
        
        fetch(`/api/streams/${streamId}/start`, {
            method: 'POST'
        })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                loadStreams();
            } else {
                alert('Erreur: ' + data.error);
            }
        })
        .catch(error => {
            console.error('Erreur lors du démarrage du flux:', error);
            alert('Erreur lors du démarrage du flux');
        });
    }

    // Fonction pour arrêter un flux
    function stopStream(event) {
        const streamId = event.target.getAttribute('data-id');
        
        fetch(`/api/streams/${streamId}/stop`, {
            method: 'POST'
        })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                loadStreams();
            } else {
                alert('Erreur: ' + data.error);
            }
        })
        .catch(error => {
            console.error('Erreur lors de l\'arrêt du flux:', error);
            alert('Erreur lors de l\'arrêt du flux');
        });
    }

    // Fonction pour éditer un flux
    function editStream(event) {
        const streamId = event.target.getAttribute('data-id');
        
        fetch(`/api/streams/${streamId}`)
            .then(response => response.json())
            .then(stream => {
                document.getElementById('streamId').value = stream.id;
                document.getElementById('streamName').value = stream.name;
                document.getElementById('hlsInput').value = stream.hlsInput;
                document.getElementById('multicastOutput').value = stream.multicastOutput;
                document.getElementById('multicastPort').value = stream.multicastPort;
                document.getElementById('bufferSize').value = stream.bufferSize;
                document.getElementById('autoStart').checked = false;
                
                document.getElementById('addStreamModalLabel').textContent = 'Éditer le flux';
                addStreamModal.show();
            })
            .catch(error => {
                console.error('Erreur lors du chargement du flux pour édition:', error);
                alert('Erreur lors du chargement du flux');
            });
    }

    // Fonction pour supprimer un flux
    function deleteStream(event) {
        if (!confirm('Voulez-vous vraiment supprimer ce flux ?')) {
            return;
        }
        
        const streamId = event.target.getAttribute('data-id');
        
        fetch(`/api/streams/${streamId}`, {
            method: 'DELETE'
        })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                loadStreams();
            } else {
                alert('Erreur: ' + data.error);
            }
        })
        .catch(error => {
            console.error('Erreur lors de la suppression du flux:', error);
            alert('Erreur lors de la suppression du flux');
        });
    }

    // Fonction pour afficher les détails d'un flux
    function viewStreamDetails(event) {
        const streamId = event.target.getAttribute('data-id');
        
        Promise.all([
            fetch(`/api/streams/${streamId}`).then(response => response.json()),
            fetch(`/api/streams/${streamId}/stats`).then(response => response.json())
        ])
        .then(([streamInfo, streamStats]) => {
            streamDetails.innerHTML = `
                <h4>${streamInfo.name}</h4>
                <div class="detail-row">
                    <div class="detail-label">ID:</div>
                    <div class="detail-value">${streamInfo.id}</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">Statut:</div>
                    <div class="detail-value">
                        <span class="badge ${streamInfo.running ? 'bg-success' : 'bg-secondary'}">
                            ${streamInfo.running ? 'En cours' : 'Arrêté'}
                        </span>
                    </div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">URL d'entrée HLS:</div>
                    <div class="detail-value">${streamInfo.hlsInput}</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">Sortie Multicast:</div>
                    <div class="detail-value">${streamInfo.multicastOutput}:${streamInfo.multicastPort}</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">Taille du buffer:</div>
                    <div class="detail-value">${streamInfo.bufferSize} segments</div>
                </div>
                
                <h4 class="mt-4">Statistiques</h4>
                <div class="detail-row">
                    <div class="detail-label">Résolution:</div>
                    <div class="detail-value">${streamStats.width}x${streamStats.height}</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">Codecs:</div>
                    <div class="detail-value">${streamStats.codecs}</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">Bande passante:</div>
                    <div class="detail-value">${(streamStats.bandwidth / 1000000).toFixed(2)} Mbps</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">Débit actuel:</div>
                    <div class="detail-value">${(streamStats.currentBitrate / 1000000).toFixed(2)} Mbps</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">Segments traités:</div>
                    <div class="detail-value">${streamStats.segmentsProcessed}</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">Discontinuités détectées:</div>
                    <div class="detail-value">${streamStats.discontinuitiesDetected}</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">État du buffer:</div>
                    <div class="detail-value">${streamStats.bufferSize} / ${streamStats.bufferCapacity} segments</div>
                </div>
                <div class="detail-row">
                    <div class="detail-label">Paquets transmis:</div>
                    <div class="detail-value">${streamStats.packetsTransmitted}</div>
                </div>
            `;
            
            streamDetailsModal.show();
        })
        .catch(error => {
            console.error('Erreur lors du chargement des détails du flux:', error);
            alert('Erreur lors du chargement des détails du flux');
        });
    }

    // Fonction pour obtenir la classe Bootstrap selon le niveau d'alerte
    function getLevelClass(level) {
        switch (level.toLowerCase()) {
            case 'info': return 'info';
            case 'warning': return 'warning';
            case 'error': return 'danger';
            default: return 'secondary';
        }
    }
});
