// État global de l'application
const appState = {
    activeSection: 'streams',
    streams: [],
    alerts: [],
    serverStatus: 'loading',
    settings: {
        server: {},
        logging: {}
    },
    alertFilters: {
        levels: ['INFO', 'WARNING', 'ERROR'],
        component: '',
        text: ''
    }
};

// Sélecteurs DOM fréquemment utilisés
const dom = {
    statusIndicator: document.getElementById('status-indicator'),
    statusText: document.getElementById('status-text'),
    sections: {
        streams: document.getElementById('streams-section'),
        alerts: document.getElementById('alerts-section'),
        settings: document.getElementById('settings-section')
    },
    streamContainer: document.getElementById('streams-container'),
    alertsContainer: document.getElementById('alerts-container'),
    streamModal: document.getElementById('stream-modal'),
    streamForm: document.getElementById('stream-form'),
    serverForm: document.getElementById('server-settings-form'),
    loggingForm: document.getElementById('logging-settings-form')
};

// Initialisation de l'application
function initApp() {
    // Attacher les gestionnaires d'événements
    attachEventListeners();
    
    // Charger les données initiales
    loadServerStatus();
    loadStreams();
    loadAlerts();
    loadSettings();
    
    // Configurer des rafraîchissements périodiques
    setInterval(loadServerStatus, 5000);
    setInterval(loadStreams, 5000);
    setInterval(loadAlerts, 5000);
}

// Attacher tous les gestionnaires d'événements
function attachEventListeners() {
    // Navigation
    document.querySelectorAll('nav a').forEach(link => {
        link.addEventListener('click', function(e) {
            e.preventDefault();
            const section = this.getAttribute('data-section');
            switchSection(section);
        });
    });
    
    // Gestion des flux
    document.getElementById('add-stream-btn').addEventListener('click', showAddStreamModal);
    document.getElementById('stream-modal-cancel').addEventListener('click', closeStreamModal);
    document.getElementById('stream-form').addEventListener('submit', handleStreamFormSubmit);
    document.getElementById('stream-search').addEventListener('input', filterStreams);
    
    // Gestion des alertes
    document.querySelectorAll('.alert-level-filter').forEach(checkbox => {
        checkbox.addEventListener('change', updateAlertFilters);
    });
    document.getElementById('alert-search').addEventListener('input', updateAlertFilters);
    document.getElementById('alert-component-filter').addEventListener('change', updateAlertFilters);
    document.getElementById('export-json-btn').addEventListener('click', () => exportAlerts('json'));
    document.getElementById('export-csv-btn').addEventListener('click', () => exportAlerts('csv'));
    
    // Gestion des paramètres
    document.getElementById('server-settings-form').addEventListener('submit', handleServerSettingsSubmit);
    document.getElementById('logging-settings-form').addEventListener('submit', handleLoggingSettingsSubmit);
    
    // Toggle de la journalisation de fichier
    document.getElementById('log-file-enabled').addEventListener('change', function() {
        const fileGroups = document.querySelectorAll('.log-file-group');
        for (const group of fileGroups) {
            group.style.display = this.checked ? 'block' : 'none';
        }
    });
}

// Changer de section active
function switchSection(sectionId) {
    // Mettre à jour la navigation
    document.querySelectorAll('nav a').forEach(link => {
        link.classList.toggle('active', link.getAttribute('data-section') === sectionId);
    });
    
    // Mettre à jour les sections
    for (const [id, element] of Object.entries(dom.sections)) {
        element.classList.toggle('active', id === sectionId);
    }
    
    appState.activeSection = sectionId;
}

// Charger le statut du serveur
async function loadServerStatus() {
    try {
        const response = await fetch('/api/status');
        if (!response.ok) throw new Error('Erreur lors du chargement du statut');
        
        const data = await response.json();
        
        appState.serverStatus = data.status === 'running' ? 'online' : 'offline';
        dom.statusIndicator.className = appState.serverStatus;
        dom.statusText.textContent = appState.serverStatus === 'online' ? 'En ligne' : 'Hors ligne';
    } catch (error) {
        console.error('Erreur lors du chargement du statut:', error);
        appState.serverStatus = 'offline';
        dom.statusIndicator.className = 'offline';
        dom.statusText.textContent = 'Hors ligne';
    }
}

// Charger la liste des flux
async function loadStreams() {
    try {
        const response = await fetch('/api/streams');
        if (!response.ok) throw new Error('Erreur lors du chargement des flux');
        
        const streams = await response.json();
        appState.streams = streams;
        
        renderStreams();
    } catch (error) {
        console.error('Erreur lors du chargement des flux:', error);
        dom.streamContainer.innerHTML = '<div class="error">Erreur lors du chargement des flux</div>';
    }
}

// Afficher la liste des flux
function renderStreams() {
    const searchTerm = document.getElementById('stream-search').value.toLowerCase();
    const filteredStreams = appState.streams.filter(stream => 
        stream.name.toLowerCase().includes(searchTerm) || 
        stream.hlsInput.toLowerCase().includes(searchTerm) ||
        stream.mcastOutput.toLowerCase().includes(searchTerm)
    );
    
    if (filteredStreams.length === 0) {
        dom.streamContainer.innerHTML = '<div class="no-data">Aucun flux trouvé</div>';
        return;
    }
    
    dom.streamContainer.innerHTML = filteredStreams.map(stream => `
        <div class="stream-card" data-id="${stream.id}">
            <div class="stream-card-header">
                <div class="stream-card-title">${stream.name}</div>
                <span class="stream-status ${stream.status}">${stream.status === 'running' ? 'En cours' : 'Arrêté'}</span>
            </div>
            <div class="stream-card-content">
                <p><span class="label">HLS:</span> ${stream.hlsInput}</p>
                <p><span class="label">Multicast:</span> ${stream.mcastOutput}:${stream.mcastPort}</p>
                <p><span class="label">Buffer:</span> ${stream.bufferSize} segments</p>
                
                ${stream.status === 'running' && stream.stats ? `
                <div class="stream-stats">
                    <div class="stream-stats-item">
                        <span>Segments reçus:</span>
                        <span>${stream.stats.segmentsReceived}</span>
                    </div>
                    <div class="stream-stats-item">
                        <span>Segments envoyés:</span>
                        <span>${stream.stats.segmentsSent}</span>
                    </div>
                    <div class="stream-stats-item">
                        <span>Discontinuités:</span>
                        <span>${stream.stats.discontinuities}</span>
                    </div>
                    <div class="stream-stats-item">
                        <span>Débit moyen:</span>
                        <span>${(stream.stats.averageBitrate / 1000).toFixed(2)} Mbps</span>
                    </div>
                    <div class="stream-stats-item">
                        <span>Niveau buffer:</span>
                        <span>${(stream.stats.bufferLevel * 100).toFixed(1)}%</span>
                    </div>
                </div>
                ` : ''}
            </div>
            <div class="stream-card-actions">
                ${stream.status === 'running' ? 
                    `<button class="btn secondary stop-stream-btn" data-id="${stream.id}">Arrêter</button>` : 
                    `<button class="btn primary start-stream-btn" data-id="${stream.id}">Démarrer</button>`
                }
                <button class="btn secondary edit-stream-btn" data-id="${stream.id}">Modifier</button>
                <button class="btn secondary delete-stream-btn" data-id="${stream.id}">Supprimer</button>
            </div>
        </div>
    `).join('');
    
    // Attacher les gestionnaires d'événements aux boutons
    document.querySelectorAll('.start-stream-btn').forEach(btn => {
        btn.addEventListener('click', () => startStream(btn.getAttribute('data-id')));
    });
    
    document.querySelectorAll('.stop-stream-btn').forEach(btn => {
        btn.addEventListener('click', () => stopStream(btn.getAttribute('data-id')));
    });
    
    document.querySelectorAll('.edit-stream-btn').forEach(btn => {
        btn.addEventListener('click', () => showEditStreamModal(btn.getAttribute('data-id')));
    });
    
    document.querySelectorAll('.delete-stream-btn').forEach(btn => {
        btn.addEventListener('click', () => deleteStream(btn.getAttribute('data-id')));
    });
}

// Filtrer les flux
function filterStreams() {
    renderStreams();
}

// Afficher le modal d'ajout de flux
function showAddStreamModal() {
    dom.streamForm.reset();
    document.getElementById('stream-id').value = '';
    document.getElementById('stream-modal-title').textContent = 'Ajouter un flux';
    dom.streamModal.classList.add('show');
}

// Afficher le modal de modification de flux
function showEditStreamModal(streamId) {
    const stream = appState.streams.find(s => s.id === streamId);
    if (!stream) return;
    
    document.getElementById('stream-id').value = stream.id;
    document.getElementById('stream-name').value = stream.name;
    document.getElementById('hls-input').value = stream.hlsInput;
    document.getElementById('mcast-output').value = stream.mcastOutput;
    document.getElementById('mcast-port').value = stream.mcastPort;
    document.getElementById('buffer-size').value = stream.bufferSize;
    document.getElementById('stream-enabled').checked = stream.enabled;
    
    document.getElementById('stream-modal-title').textContent = 'Modifier le flux';
    dom.streamModal.classList.add('show');
}

// Fermer le modal de flux
function closeStreamModal() {
    dom.streamModal.classList.remove('show');
}

// Gérer la soumission du formulaire de flux
async function handleStreamFormSubmit(e) {
    e.preventDefault();
    
    const formData = new FormData(dom.streamForm);
    const streamData = {
        name: formData.get('name'),
        hlsInput: formData.get('hlsInput'),
        mcastOutput: formData.get('mcastOutput'),
        mcastPort: parseInt(formData.get('mcastPort')),
        bufferSize: parseInt(formData.get('bufferSize')),
        enabled: formData.has('enabled')
    };
    
    const streamId = formData.get('id');
    let url = '/api/streams';
    let method = 'POST';
    
    if (streamId) {
        url = `/api/streams/${streamId}`;
        method = 'PUT';
        streamData.id = streamId;
    }
    
    try {
        const response = await fetch(url, {
            method,
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(streamData)
        });
        
        if (!response.ok) {
            const error = await response.json();
            throw new Error(error.error || 'Erreur lors de la sauvegarde du flux');
        }
        
        closeStreamModal();
        loadStreams();
    } catch (error) {
        console.error('Erreur lors de la sauvegarde du flux:', error);
        alert(`Erreur: ${error.message}`);
    }
}

// Démarrer un flux
async function startStream(streamId) {
    try {
        const response = await fetch(`/api/streams/${streamId}/start`, {
            method: 'POST'
        });
        
        if (!response.ok) {
            const error = await response.json();
            throw new Error(error.error || 'Erreur lors du démarrage du flux');
        }
        
        loadStreams();
    } catch (error) {
        console.error('Erreur lors du démarrage du flux:', error);
        alert(`Erreur: ${error.message}`);
    }
}

// Arrêter un flux
async function stopStream(streamId) {
    try {
        const response = await fetch(`/api/streams/${streamId}/stop`, {
            method: 'POST'
        });
        
        if (!response.ok) {
            const error = await response.json();
            throw new Error(error.error || 'Erreur lors de l\'arrêt du flux');
        }
        
        loadStreams();
    } catch (error) {
        console.error('Erreur lors de l\'arrêt du flux:', error);
        alert(`Erreur: ${error.message}`);
    }
}

// Supprimer un flux
async function deleteStream(streamId) {
    if (!confirm('Êtes-vous sûr de vouloir supprimer ce flux?')) {
        return;
    }
    
    try {
        const response = await fetch(`/api/streams/${streamId}`, {
            method: 'DELETE'
        });
        
        if (!response.ok) {
            const error = await response.json();
            throw new Error(error.error || 'Erreur lors de la suppression du flux');
        }
        
        loadStreams();
    } catch (error) {
        console.error('Erreur lors de la suppression du flux:', error);
        alert(`Erreur: ${error.message}`);
    }
}

// Charger les alertes
async function loadAlerts() {
    const queryParams = new URLSearchParams();
    
    // Ajouter les niveaux d'alerte
    queryParams.append('levels', appState.alertFilters.levels.join(','));
    
    // Ajouter les autres filtres
    if (appState.alertFilters.component) {
        queryParams.append('component', appState.alertFilters.component);
    }
    
    if (appState.alertFilters.text) {
        queryParams.append('text', appState.alertFilters.text);
    }
    
    try {
        const response = await fetch(`/api/alerts?${queryParams.toString()}`);
        if (!response.ok) throw new Error('Erreur lors du chargement des alertes');
        
        const alerts = await response.json();
        appState.alerts = alerts;
        
        renderAlerts();
    } catch (error) {
        console.error('Erreur lors du chargement des alertes:', error);
        dom.alertsContainer.innerHTML = '<div class="error">Erreur lors du chargement des alertes</div>';
    }
}

// Afficher les alertes
function renderAlerts() {
    if (appState.alerts.length === 0) {
        dom.alertsContainer.innerHTML = '<div class="no-data">Aucune alerte trouvée</div>';
        return;
    }
    
    dom.alertsContainer.innerHTML = appState.alerts.map(alert => `
        <div class="alert-item">
            <div class="alert-level ${alert.level}">${alert.level}</div>
            <div class="alert-content">
                <div class="alert-message">${alert.message}</div>
                <div class="alert-meta">
                    <span class="alert-component">${alert.component}</span>
                    <span class="alert-time">${new Date(alert.timestamp).toLocaleString()}</span>
                    ${alert.persistent ? `
                        <button class="alert-resolve btn secondary" data-id="${alert.id}">Résoudre</button>
                    ` : ''}
                </div>
            </div>
        </div>
    `).join('');
    
    // Attacher les gestionnaires d'événements aux boutons de résolution
    document.querySelectorAll('.alert-resolve').forEach(btn => {
        btn.addEventListener('click', () => resolveAlert(btn.getAttribute('data-id')));
    });
}

// Mettre à jour les filtres d'alertes
function updateAlertFilters() {
    // Récupérer les niveaux d'alerte sélectionnés
    const levels = [];
    document.querySelectorAll('.alert-level-filter').forEach(checkbox => {
        if (checkbox.checked) {
            levels.push(checkbox.getAttribute('data-level'));
        }
    });
    
    appState.alertFilters.levels = levels;
    
    // Récupérer le filtre de composant
    appState.alertFilters.component = document.getElementById('alert-component-filter').value;
    
    // Récupérer le filtre de texte
    appState.alertFilters.text = document.getElementById('alert-search').value;
    
    // Recharger les alertes
    loadAlerts();
}

// Résoudre une alerte
async function resolveAlert(alertId) {
    try {
        const response = await fetch(`/api/alerts/${alertId}/resolve`, {
            method: 'POST'
        });
        
        if (!response.ok) {
            const error = await response.json();
            throw new Error(error.error || 'Erreur lors de la résolution de l\'alerte');
        }
        
        loadAlerts();
    } catch (error) {
        console.error('Erreur lors de la résolution de l\'alerte:', error);
        alert(`Erreur: ${error.message}`);
    }
}

// Exporter les alertes
function exportAlerts(format) {
    const queryParams = new URLSearchParams();
    
    // Ajouter les niveaux d'alerte
    queryParams.append('levels', appState.alertFilters.levels.join(','));
    
    // Ajouter les autres filtres
    if (appState.alertFilters.component) {
        queryParams.append('component', appState.alertFilters.component);
    }
    
    if (appState.alertFilters.text) {
        queryParams.append('text', appState.alertFilters.text);
    }
    
    // Ajouter le format
    queryParams.append('format', format);
    
    // Ouvrir l'URL dans un nouvel onglet
    window.open(`/api/alerts/export?${queryParams.toString()}`, '_blank');
}

// Charger les paramètres
async function loadSettings() {
    try {
        const response = await fetch('/api/status');
        if (!response.ok) throw new Error('Erreur lors du chargement des paramètres');
        
        const data = await response.json();
        
        // Remplir les formulaires
        if (data.server) {
            appState.settings.server = data.server;
            fillFormWithObject(dom.serverForm, data.server);
        }
        
        if (data.logging) {
            appState.settings.logging = data.logging;
            fillFormWithObject(dom.loggingForm, data.logging);
            
            // Mettre à jour l'affichage des groupes de fichiers
            const fileEnabled = document.getElementById('log-file-enabled').checked;
            document.querySelectorAll('.log-file-group').forEach(group => {
                group.style.display = fileEnabled ? 'block' : 'none';
            });
        }
    } catch (error) {
        console.error('Erreur lors du chargement des paramètres:', error);
    }
}

// Remplir un formulaire avec un objet
function fillFormWithObject(form, obj) {
    for (const [key, value] of Object.entries(obj)) {
        const input = form.elements[key];
        if (input) {
            if (input.type === 'checkbox') {
                input.checked = value;
            } else {
                input.value = value;
            }
        }
    }
}

// Gérer la soumission du formulaire de paramètres du serveur
async function handleServerSettingsSubmit(e) {
    e.preventDefault();
    
    const formData = new FormData(dom.serverForm);
    const serverData = {
        address: formData.get('address'),
        port: parseInt(formData.get('port')),
        workerThreads: parseInt(formData.get('workerThreads'))
    };
    
    try {
        const response = await fetch('/api/settings/server', {
            method: 'PUT',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(serverData)
        });
        
        if (!response.ok) {
            const error = await response.json();
            throw new Error(error.error || 'Erreur lors de la sauvegarde des paramètres du serveur');
        }
        
        alert('Paramètres du serveur sauvegardés avec succès. Les modifications prendront effet au prochain redémarrage.');
    } catch (error) {
        console.error('Erreur lors de la sauvegarde des paramètres du serveur:', error);
        alert(`Erreur: ${error.message}`);
    }
}

// Gérer la soumission du formulaire de paramètres de journalisation
async function handleLoggingSettingsSubmit(e) {
    e.preventDefault();
    
    const formData = new FormData(dom.loggingForm);
    const loggingData = {
        level: formData.get('level'),
        console: formData.has('console'),
        file: {
            enabled: formData.has('file.enabled'),
            path: formData.get('file.path'),
            rotationSize: parseInt(formData.get('file.rotationSize')) * 1024 * 1024, // En octets
            maxFiles: parseInt(formData.get('file.maxFiles'))
        }
    };
    
    try {
        const response = await fetch('/api/settings/logging', {
            method: 'PUT',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(loggingData)
        });
        
        if (!response.ok) {
            const error = await response.json();
            throw new Error(error.error || 'Erreur lors de la sauvegarde des paramètres de journalisation');
        }
        
        alert('Paramètres de journalisation sauvegardés avec succès.');
    } catch (error) {
        console.error('Erreur lors de la sauvegarde des paramètres de journalisation:', error);
        alert(`Erreur: ${error.message}`);
    }
}

// Initialiser l'application au chargement de la page
document.addEventListener('DOMContentLoaded', initApp);
